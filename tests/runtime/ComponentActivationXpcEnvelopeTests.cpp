#include "runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"

#include <xpc/xpc.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <limits>
#include <string_view>
#include <vector>

namespace
{
std::atomic<std::int64_t> failAllocationAfter = -1;

bool shouldFailAllocation()
{
    const auto remaining = failAllocationAfter.load(std::memory_order_relaxed);
    if (remaining < 0)
        return false;
    if (failAllocationAfter.fetch_sub(1, std::memory_order_relaxed) == 0)
    {
        failAllocationAfter.store(-1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
}

void* operator new(std::size_t size)
{
    if (shouldFailAllocation())
        throw std::bad_alloc{};
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (shouldFailAllocation())
        throw std::bad_alloc{};
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
namespace activation = talas::runtime::component_activation;
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
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
    value.sharedStorageBytes = 65'536;
    value.runtimeSessionNonce.fill(0x33);
    return value;
}

activation::ComponentActivationAuthorizationV1 authorization(
    const activation::ComponentActivationSealedSessionV1& sealed)
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
    constexpr std::array<std::uint8_t, 3> entryBytes{'a', 'b', 'c'};
    value.entryDigest =
        activation::calculateComponentActivationByteDigestV1(entryBytes);
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = currentComponentTrustPolicyVersion;
    value.sampleRate = 48'000;
    value.blockFrames = 256;
    value.inputChannelCount = 1;
    value.outputChannelCount = 1;
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Scalar);
    parameter.idBytes = 4;
    std::copy_n("gain", 4, parameter.id.begin());
    parameter.scalarValue = 0.5;
    const std::array parameters{parameter};
    value.staticParameterCount = parameters.size();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    value.entryByteCount = entryBytes.size();
    value.artifactGeneration = 29;
    value.activationGeneration = 31;
    value.runtimeSessionGeneration = sealed.runtimeSessionGeneration;
    value.sharedStorageIdentity = sealed.sharedStorageIdentity;
    value.validationNonce.fill(0x5c);
    value.runtimeSessionNonce = sealed.runtimeSessionNonce;
    value.authorizationNonce.fill(0xa5);
    return value;
}

std::array<activation::ComponentActivationStaticParameterV1, 1>
staticParameters()
{
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Scalar);
    parameter.idBytes = 4;
    std::copy_n("gain", 4, parameter.id.begin());
    parameter.scalarValue = 0.5;
    return {parameter};
}

std::array<activation::ComponentActivationStaticParameterV1, 1>
resourceParameters()
{
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "table";
    constexpr std::string_view resourceId = "Tables/Curve-A";
    parameter.idBytes = parameterId.size();
    parameter.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), parameter.text.begin());
    return {parameter};
}

std::array<activation::ComponentActivationStaticParameterV1, 1>
audioParameters()
{
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Loop-A";
    parameter.idBytes = parameterId.size();
    parameter.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), parameter.text.begin());
    return {parameter};
}

activation::ComponentActivationAuthorizationV1 audioAuthorization(
    activation::ComponentActivationSealedSessionV1& sealed)
{
    sealed.supportedPermissions |= componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    auto value = authorization(sealed);
    value.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    value.declaredRoles |= componentRole(
        ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto parameters = audioParameters();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    return value;
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (std::uint32_t shift : {0U, 8U, 16U, 24U})
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> audioSourceBytes(
    std::uint32_t frameCount = 8)
{
    constexpr std::uint16_t channels = 2;
    constexpr std::uint32_t sampleRate = 48'000;
    const auto dataBytes = frameCount * channels * 2U;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44U + dataBytes);
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    appendU32(bytes, 36U + dataBytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * channels * 2U);
    appendU16(bytes, channels * 2U);
    appendU16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(bytes, dataBytes);
    bytes.resize(44U + dataBytes);
    for (std::size_t index = 44; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(index);
    return bytes;
}

activation::ComponentActivationAudioStreamGrantV1 audioStreamGrant(
    const activation::ComponentActivationAuthorizationV1& authority,
    std::span<const std::uint8_t> bytes)
{
    activation::ComponentActivationAudioStreamGrantV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::AudioStreamGrant,
        sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.signerIdentityDigest = authority.signerIdentityDigest;
    value.sourceContentDigest =
        activation::calculateComponentActivationByteDigestV1(bytes);
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.resourceGeneration = 7;
    value.sourceByteCount = bytes.size();
    value.sourceFrameCount = (bytes.size() - 44U) / 4U;
    value.maximumSourceFrameIncrement =
        activation::maximumComponentActivationAudioSourceFrameIncrementV1;
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Loop-A";
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
    std::copy(parameterId.begin(), parameterId.end(), value.parameterId.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.resourceId.begin());
    return value;
}

xpc_object_t adversarialAudioSharedMemory(
    std::span<const std::uint8_t> bytes,
    std::size_t extraPages = 0,
    std::uint8_t tailValue = 0)
{
    const auto pageBytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const auto mappedBytes =
        ((bytes.size() + pageBytes - 1U) / pageBytes + extraPages) * pageBytes;
    auto* mapping = mmap(
        nullptr,
        mappedBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_PRIVATE,
        -1,
        0);
    require(mapping != MAP_FAILED, "adversarial Audio mapping should allocate");
    std::memcpy(mapping, bytes.data(), bytes.size());
    if (tailValue != 0)
    {
        std::fill_n(
            static_cast<std::uint8_t*>(mapping) + bytes.size(),
            mappedBytes - bytes.size(),
            tailValue);
    }
    auto memory = xpc_shmem_create(mapping, mappedBytes);
    require(memory != nullptr, "adversarial Audio XPC memory should allocate");
    require(munmap(mapping, mappedBytes) == 0,
        "adversarial Audio source mapping should release");
    return memory;
}

bool isReadOnlyNonExecutable(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return false;
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(
        bytes.data());
    mach_vm_size_t regionBytes = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(
            mach_task_self(),
            &address,
            &regionBytes,
            VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info),
            &infoCount,
            &object)
        != KERN_SUCCESS)
    {
        return false;
    }
    if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
    return (info.protection & VM_PROT_READ) != 0
        && (info.protection & VM_PROT_WRITE) == 0
        && (info.protection & VM_PROT_EXECUTE) == 0;
}

activation::ComponentActivationAuthorizationV1 resourceAuthorization(
    activation::ComponentActivationSealedSessionV1& sealed)
{
    sealed.supportedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    auto value = authorization(sealed);
    value.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    value.declaredRoles |= componentRole(
        ComponentExecutionRole::ImmutableResourceConsumer);
    const auto parameters = resourceParameters();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    return value;
}

std::array<std::uint8_t, 3U * sizeof(double)> resourcePayload()
{
    constexpr std::array values{0.25, 0.5, 0.75};
    std::array<std::uint8_t, 3U * sizeof(double)> bytes{};
    require(
        activation::encodeComponentActivationImmutableTableV1(values, bytes)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "resource fixture should encode canonically");
    return bytes;
}

activation::ComponentActivationImmutableResourceGrantV1 resourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority,
    std::span<const std::uint8_t> bytes)
{
    activation::ComponentActivationImmutableResourceGrantV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::ImmutableResourceGrant,
        sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.sourceContentDigest.fill(0x61);
    value.mediatedPayloadDigest =
        activation::calculateComponentActivationByteDigestV1(bytes);
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.resourceGeneration = 7;
    value.elementCount = 3;
    value.byteCount = bytes.size();
    constexpr std::string_view parameterId = "table";
    constexpr std::string_view resourceId = "Tables/Curve-A";
    value.parameterIdBytes = parameterId.size();
    value.resourceIdBytes = resourceId.size();
    value.resourceKind = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceKindV1::Table);
    value.elementFormat = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceElementFormatV1::
            Float64LittleEndian);
    std::copy(parameterId.begin(), parameterId.end(), value.parameterId.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.resourceId.begin());
    return value;
}

std::array<std::uint8_t, 5> binaryResourcePayload()
{
    return {0x00, 0x7f, 0x80, 0xfe, 0xff};
}

activation::ComponentActivationImmutableResourceGrantV1 binaryResourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority,
    std::span<const std::uint8_t> bytes)
{
    auto value = resourceGrant(authority, bytes);
    value.sourceContentDigest = value.mediatedPayloadDigest;
    value.elementCount = bytes.size();
    value.byteCount = bytes.size();
    value.resourceKind = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceKindV1::Binary);
    value.elementFormat = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceElementFormatV1::
            RawBytes);
    return value;
}

template <typename Record>
std::array<std::uint8_t, componentTrustDigestBytes> recordDigest(
    const Record& record)
{
    return activation::calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&record),
            sizeof(record)});
}

std::uint64_t setTransportBytes(std::uint64_t sourceBytes)
{
    constexpr auto page =
        activation::maximumComponentActivationResourceSetTransportPageBytesV1;
    return ((sourceBytes + page - 1U) / page) * page;
}

struct ResourceSetEnvelopeFixture
{
    activation::ComponentActivationSealedSessionV1 sealed = sealedSession();
    activation::ComponentActivationAuthorizationV1 authority;
    std::array<activation::ComponentActivationStaticParameterV1, 3>
        parameters{};
    std::array<std::uint8_t, 3U * sizeof(double)> tableBytes =
        resourcePayload();
    std::array<std::uint8_t, 5> binaryBytes = binaryResourcePayload();
    std::vector<std::uint8_t> audioBytes = audioSourceBytes();
    activation::ComponentActivationImmutableResourceGrantV1 tableGrant;
    activation::ComponentActivationImmutableResourceGrantV1 binaryGrant;
    activation::ComponentActivationAudioStreamGrantV1 audioGrant;
    activation::ComponentActivationResourceSetGrantV1 setGrant;
    std::array<activation::ComponentActivationResourceSetMemberV1, 3>
        members{};
    std::array<activation::ComponentActivationXpcResourceSetSourceV1, 3>
        sources{};
    xpc_object_t audioMemory = nullptr;

    ResourceSetEnvelopeFixture()
    {
        sealed.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
        sealed.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
        authority = authorization(sealed);
        authority.requestedPermissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
        authority.requestedPermissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
        authority.declaredRoles |= componentRole(
            ComponentExecutionRole::ImmutableResourceConsumer);
        authority.declaredRoles |= componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);

        const std::array<std::string_view, 3> parameterIds{
            "table", "binary", "audio"};
        const std::array<std::string_view, 3> resourceIds{
            "Tables/Curve-A", "Binary/Blob-A", "Audio/Loop-A"};
        for (std::size_t index = 0; index < parameters.size(); ++index)
        {
            auto& parameter = parameters[index];
            parameter.kind = static_cast<std::uint32_t>(
                activation::ComponentActivationStaticParameterKindV1::
                    Resource);
            parameter.idBytes = parameterIds[index].size();
            parameter.textBytes = resourceIds[index].size();
            std::copy(
                parameterIds[index].begin(),
                parameterIds[index].end(),
                parameter.id.begin());
            std::copy(
                resourceIds[index].begin(),
                resourceIds[index].end(),
                parameter.text.begin());
        }
        authority.staticParameterCount = parameters.size();
        authority.staticParameterDigest =
            activation::calculateComponentActivationStaticParameterDigestV1(
                parameters);

        tableGrant = resourceGrant(authority, tableBytes);
        binaryGrant = binaryResourceGrant(authority, binaryBytes);
        constexpr std::string_view binaryParameter = "binary";
        constexpr std::string_view binaryResource = "Binary/Blob-A";
        binaryGrant.parameterId.fill(0);
        binaryGrant.resourceId.fill(0);
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
        audioGrant = audioStreamGrant(authority, audioBytes);

        const std::array<std::uint64_t, 3> sourceBytes{
            tableGrant.byteCount,
            binaryGrant.byteCount,
            audioGrant.sourceByteCount};
        const std::array<std::uint64_t, 3> generations{
            tableGrant.resourceGeneration,
            binaryGrant.resourceGeneration,
            audioGrant.resourceGeneration};
        const std::array<std::array<std::uint8_t, componentTrustDigestBytes>, 3>
            sourceDigests{
                tableGrant.sourceContentDigest,
                binaryGrant.sourceContentDigest,
                audioGrant.sourceContentDigest};
        const std::array<std::array<std::uint8_t, componentTrustDigestBytes>, 3>
            mediatedDigests{
                tableGrant.mediatedPayloadDigest,
                binaryGrant.mediatedPayloadDigest,
                audioGrant.sourceContentDigest};
        const std::array<std::array<std::uint8_t, componentTrustDigestBytes>, 3>
            grantDigests{
                recordDigest(tableGrant),
                recordDigest(binaryGrant),
                recordDigest(audioGrant)};
        const std::array kinds{
            activation::ComponentActivationResourceSetMemberKindV1::Table,
            activation::ComponentActivationResourceSetMemberKindV1::Binary,
            activation::ComponentActivationResourceSetMemberKindV1::Audio};
        for (std::size_t index = 0; index < members.size(); ++index)
        {
            auto& member = members[index];
            member.header = activation::componentActivationHeaderV1(
                activation::ComponentActivationRecordKindV1::
                    ResourceSetMember,
                sizeof(member));
            member.memberGrantDigest = grantDigests[index];
            member.sourceContentDigest = sourceDigests[index];
            member.mediatedPayloadDigest = mediatedDigests[index];
            member.resourceGeneration = generations[index];
            member.sourceByteCount = sourceBytes[index];
            member.transportByteCount = setTransportBytes(sourceBytes[index]);
            member.privateMappedByteCount = member.transportByteCount;
            member.workingByteCount = kinds[index]
                    == activation::ComponentActivationResourceSetMemberKindV1::
                        Audio
                ? activation::maximumComponentActivationAudioStreamWorkingBytesV1
                : 0;
            member.parameterIdBytes = parameterIds[index].size();
            member.resourceIdBytes = resourceIds[index].size();
            member.memberKind = static_cast<std::uint32_t>(kinds[index]);
            member.memberOrdinal = index;
            member.memberCount = members.size();
            std::copy(
                parameterIds[index].begin(),
                parameterIds[index].end(),
                member.parameterId.begin());
            std::copy(
                resourceIds[index].begin(),
                resourceIds[index].end(),
                member.resourceId.begin());
        }

        setGrant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::ResourceSetGrant,
            sizeof(setGrant));
        setGrant.contentDigest = authority.contentDigest;
        setGrant.validationNonce = authority.validationNonce;
        setGrant.runtimeSessionNonce = authority.runtimeSessionNonce;
        setGrant.authorizationNonce = authority.authorizationNonce;
        setGrant.artifactGeneration = authority.artifactGeneration;
        setGrant.activationGeneration = authority.activationGeneration;
        setGrant.runtimeSessionGeneration = authority.runtimeSessionGeneration;
        setGrant.sharedStorageIdentity = authority.sharedStorageIdentity;
        setGrant.memberCount = members.size();
        setGrant.audioStreamCount = 1;
        setGrant.profileVersion =
            activation::componentActivationResourceSetProfileVersionV1;
        for (const auto& member : members)
        {
            setGrant.aggregateSourceBytes += member.sourceByteCount;
            setGrant.aggregateTransportBytes += member.transportByteCount;
            setGrant.aggregatePrivateMappedBytes +=
                member.privateMappedByteCount;
            setGrant.aggregateWorkingBytes += member.workingByteCount;
        }
        setGrant.resourceSetDigest =
            activation::calculateComponentActivationResourceSetDigestV1(
                setGrant, members);
        for (auto& member : members)
            member.resourceSetDigest = setGrant.resourceSetDigest;

        audioMemory =
            activation::createComponentActivationXpcAudioSourceSharedMemoryV1(
                audioGrant, audioBytes);
        require(audioMemory != nullptr,
            "resource-set Audio memory should allocate");
        sources[0].immutableGrant = &tableGrant;
        sources[0].immutableBytes = tableBytes;
        sources[1].immutableGrant = &binaryGrant;
        sources[1].immutableBytes = binaryBytes;
        sources[2].audioGrant = &audioGrant;
        sources[2].audioSourceSharedMemory = audioMemory;
    }

    ~ResourceSetEnvelopeFixture()
    {
        if (audioMemory != nullptr)
            xpc_release(audioMemory);
    }
};

activation::ComponentActivationTransitionV1 accepted(
    const activation::ComponentActivationAuthorizationV1& authority)
{
    activation::ComponentActivationTransitionV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
        sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.state = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeStateV1::Authorized);
    value.diagnostic = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeDiagnosticV1::Ready);
    value.sequence = 1;
    return value;
}

activation::ComponentActivationTransitionV1 sealedReady(
    const activation::ComponentActivationSealedSessionV1& sealed)
{
    activation::ComponentActivationTransitionV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::SealedReady,
        sizeof(value));
    value.runtimeSessionGeneration = sealed.runtimeSessionGeneration;
    value.sharedStorageIdentity = sealed.sharedStorageIdentity;
    value.runtimeSessionNonce = sealed.runtimeSessionNonce;
    value.state = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeStateV1::Sealed);
    value.diagnostic = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeDiagnosticV1::Ready);
    value.sequence = 1;
    return value;
}

void testExactControlRecordsRoundTripWithoutComponentBytes()
{
    const auto sealed = sealedSession();
    auto sealedMessage =
        activation::createComponentActivationXpcSealedSessionV1(sealed);
    require(sealedMessage != nullptr, "sealed session should encode");
    activation::ComponentActivationSealedSessionV1 decodedSealed;
    require(
        activation::decodeComponentActivationXpcSealedSessionV1(
            sealedMessage, decodedSealed)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(&sealed, &decodedSealed, sizeof(sealed)) == 0,
        "sealed session should round-trip exactly");
    xpc_release(sealedMessage);

    const auto authority = authorization(sealed);
    auto authorizationMessage =
        activation::createComponentActivationXpcAuthorizationV1(
            authority, sealed);
    require(
        authorizationMessage != nullptr
            && xpc_dictionary_get_count(authorizationMessage) == 1,
        "authorization control must contain exactly one typed record and no bytes");
    activation::ComponentActivationAuthorizationV1 decodedAuthority;
    require(
        activation::decodeComponentActivationXpcAuthorizationV1(
            authorizationMessage, sealed, decodedAuthority)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &authority, &decodedAuthority, sizeof(authority)) == 0,
        "authorization should round-trip exactly");
    xpc_release(authorizationMessage);

    const auto command = activation::makeComponentActivationCommandV1(
        authority,
        activation::ComponentActivationRecordKindV1::Teardown,
        7);
    auto commandMessage = activation::createComponentActivationXpcCommandV1(
        command,
        activation::ComponentActivationRecordKindV1::Teardown,
        authority);
    require(
        commandMessage != nullptr
            && xpc_dictionary_get_count(commandMessage) == 1,
        "teardown control must contain exactly one authority-bound command");
    activation::ComponentActivationCommandV1 decodedCommand;
    require(
        activation::decodeComponentActivationXpcCommandV1(
            commandMessage,
            activation::ComponentActivationRecordKindV1::Teardown,
            authority,
            decodedCommand)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(&command, &decodedCommand, sizeof(command)) == 0,
        "an exact teardown command should round-trip");
    xpc_release(commandMessage);

    auto transitionMessage = xpc_dictionary_create_empty();
    const auto transition = accepted(authority);
    require(
        activation::writeComponentActivationXpcTransitionV1(
            transitionMessage,
            transition,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority),
        "validated transition should encode");
    activation::ComponentActivationTransitionV1 decodedTransition;
    require(
        activation::decodeComponentActivationXpcTransitionV1(
            transitionMessage,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority,
            decodedTransition)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &transition, &decodedTransition, sizeof(transition)) == 0,
        "transition should round-trip exactly");
    xpc_release(transitionMessage);
}

void testMalformedAndPreauthorizationPayloadEnvelopesReject()
{
    activation::ComponentActivationSealedSessionV1 decoded;
    auto text = xpc_string_create("not activation control");
    require(
        activation::decodeComponentActivationXpcSealedSessionV1(text, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ObjectTypeInvalid,
        "non-dictionary control must reject");
    xpc_release(text);

    const auto sealed = sealedSession();
    auto message = activation::createComponentActivationXpcSealedSessionV1(
        sealed);
    const std::uint8_t componentByte = 0x7f;
    xpc_dictionary_set_data(
        message, "talas.component-activation.entry.v1", &componentByte, 1);
    require(
        activation::decodeComponentActivationXpcSealedSessionV1(
            message, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid,
        "component bytes attached before authorization must reject");
    xpc_release(message);

    const auto authority = authorization(sealed);
    message = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.authorization.v1",
        &authority,
        sizeof(authority) - 1U);
    activation::ComponentActivationAuthorizationV1 decodedAuthority;
    require(
        activation::decodeComponentActivationXpcAuthorizationV1(
            message, sealed, decodedAuthority)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AuthorizationSizeInvalid,
        "truncated authorization must reject before copying");
    xpc_release(message);
}

void testExactAuthorizedLoadPayloadIsDigestAndCountBound()
{
    const auto sealed = sealedSession();
    const auto authority = authorization(sealed);
    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    const auto parameters = staticParameters();
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    char fileTemplate[] = "/private/tmp/talas-activation-envelope.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0, "materialization file should allocate");
    require(unlink(fileTemplate) == 0, "materialization file should unlink");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(
        materializationFile != nullptr,
        "materialization descriptor should become XPC data");

    auto message = activation::createComponentActivationXpcLoadPayloadV1(
        authority,
        sealed,
        manifestBytes,
        entry,
        parameters,
        materializationFile);
    require(
        message != nullptr && xpc_dictionary_get_count(message) == 5,
        "authorized load should contain exactly five typed fields");
    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &authority,
                   &decoded.authorization,
                   sizeof(authority)) == 0
            && std::ranges::equal(decoded.canonicalManifest, manifestBytes)
            && std::ranges::equal(decoded.entryBytes, entry)
            && decoded.staticParameters.size() == parameters.size()
            && std::memcmp(
                   decoded.staticParameters.data(),
                   parameters.data(),
                   sizeof(parameters)) == 0
            && decoded.materializationFile != nullptr
            && xpc_get_type(decoded.materializationFile) == XPC_TYPE_FD,
        "authorized load should round-trip exact immutable bytes");
    xpc_release(message);

    message = activation::createComponentActivationXpcLoadPayloadV1(
        authority,
        sealed,
        manifestBytes,
        entry,
        parameters,
        materializationFile);
    const std::uint8_t extra = 1;
    xpc_dictionary_set_data(message, "unexpected", &extra, 1);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid,
        "an extra load field must reject");
    xpc_release(message);

    auto changedEntry = entry;
    changedEntry.front() ^= 0xffU;
    message = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.authorization.v1",
        &authority,
        sizeof(authority));
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.manifest.v1",
        manifestBytes.data(),
        manifestBytes.size());
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.entry.v1",
        changedEntry.data(),
        changedEntry.size());
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.parameters.v1",
        parameters.data(),
        sizeof(parameters));
    xpc_dictionary_set_value(
        message,
        "talas.component-activation.materialization-file.v1",
        materializationFile);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::EntryDigestMismatch,
        "same-sized changed entry bytes must reject by digest");
    xpc_release(message);

    auto changedParameters = parameters;
    changedParameters.front().scalarValue = 0.75;
    message = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.authorization.v1",
        &authority,
        sizeof(authority));
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.manifest.v1",
        manifestBytes.data(),
        manifestBytes.size());
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.entry.v1",
        entry.data(),
        entry.size());
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.parameters.v1",
        changedParameters.data(),
        sizeof(changedParameters));
    xpc_dictionary_set_value(
        message,
        "talas.component-activation.materialization-file.v1",
        materializationFile);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::StaticParametersDigestMismatch,
        "same-sized changed parameter values must reject by digest");
    xpc_release(message);
    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0, "materialization file should close");
}

void testSealedPrewarmCarriesOnlyExactSharedStorage()
{
    const auto sealed = sealedSession();
    const auto storageBytes = static_cast<std::size_t>(
        sealed.sharedStorageBytes);
    auto* storage = mmap(
        nullptr,
        storageBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED,
        -1,
        0);
    require(storage != MAP_FAILED, "sealed prewarm mmap should allocate");
    auto sharedMemory = xpc_shmem_create(storage, storageBytes);
    require(sharedMemory != nullptr, "sealed prewarm shmem should allocate");
    auto message = activation::createComponentActivationXpcSealedPrewarmV1(
        sealed, sharedMemory);
    require(
        message != nullptr && xpc_dictionary_get_count(message) == 2,
        "sealed prewarm should contain one session and one shmem field");
    activation::ComponentActivationSealedSessionV1 decoded;
    xpc_object_t decodedSharedMemory = nullptr;
    require(
        activation::decodeComponentActivationXpcSealedPrewarmV1(
            message, decoded, decodedSharedMemory)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(&sealed, &decoded, sizeof(sealed)) == 0
            && decodedSharedMemory != nullptr
            && xpc_get_type(decodedSharedMemory) == XPC_TYPE_SHMEM,
        "sealed prewarm should decode the exact session and shared storage");

    const std::uint8_t componentByte = 0x7f;
    xpc_dictionary_set_data(
        message, "talas.component-activation.entry.v1", &componentByte, 1);
    require(
        activation::decodeComponentActivationXpcSealedPrewarmV1(
            message, decoded, decodedSharedMemory)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid,
        "component bytes must not enter a sealed prewarm envelope");
    xpc_release(message);
    xpc_release(sharedMemory);
    require(
        munmap(storage, storageBytes) == 0,
        "sealed prewarm mmap should release");

    auto reply = xpc_dictionary_create_empty();
    const auto transition = sealedReady(sealed);
    require(
        activation::writeComponentActivationXpcSealedTransitionV1(
            reply,
            transition,
            activation::ComponentActivationRecordKindV1::SealedReady,
            sealed),
        "an exact sealed acknowledgement should encode");
    activation::ComponentActivationTransitionV1 decodedTransition;
    require(
        activation::decodeComponentActivationXpcSealedTransitionV1(
            reply,
            activation::ComponentActivationRecordKindV1::SealedReady,
            sealed,
            decodedTransition)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &transition, &decodedTransition, sizeof(transition)) == 0,
        "an exact sealed acknowledgement should round-trip");
    xpc_release(reply);
}

void testImmutableResourceLoadPayloadIsExactAndFailClosed()
{
    auto sealed = sealedSession();
    const auto authority = resourceAuthorization(sealed);
    const auto parameters = resourceParameters();
    const auto bytes = resourcePayload();
    const auto grant = resourceGrant(authority, bytes);
    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    char fileTemplate[] = "/private/tmp/talas-activation-resource-envelope.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0, "resource materialization file should allocate");
    require(unlink(fileTemplate) == 0, "resource materialization file should unlink");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(materializationFile != nullptr, "resource materialization XPC fd should allocate");

    auto message = activation::createComponentActivationXpcLoadPayloadV1(
        authority,
        sealed,
        manifestBytes,
        entry,
        parameters,
        materializationFile,
        &grant,
        bytes);
    require(
        message != nullptr && xpc_dictionary_get_count(message) == 7,
        "resource load should contain exactly seven typed fields");
    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && decoded.resourceGrant.has_value()
            && std::memcmp(
                   &*decoded.resourceGrant, &grant, sizeof(grant)) == 0
            && std::ranges::equal(decoded.resourceBytes, bytes),
        "resource load should round-trip the exact grant and mediated bytes");
    xpc_release(message);

    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile)
            == nullptr,
        "a resource authorization without its opaque grant must reject");

    auto changedBytes = bytes;
    changedBytes.front() ^= 0x01U;
    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            &grant,
            changedBytes)
            == nullptr,
        "same-sized resource mutation must reject before transport");

    auto staleGrant = grant;
    staleGrant.resourceGeneration = 0;
    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            &staleGrant,
            bytes)
            == nullptr,
        "an invalid resource generation must reject");

    const auto baseAuthority = authorization(sealedSession());
    const auto baseParameters = staticParameters();
    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            baseAuthority,
            sealedSession(),
            manifestBytes,
            entry,
            baseParameters,
            materializationFile,
            &grant,
            bytes)
            == nullptr,
        "the stateless profile must reject an injected resource payload");

    const auto exactMessage = [&] {
        return activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            &grant,
            bytes);
    };

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-grant.v1",
        &grant,
        sizeof(grant) - 1U);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceGrantSizeInvalid,
        "a truncated immutable-resource grant must reject before copying");
    xpc_release(message);

    message = exactMessage();
    auto invalidGeneration = grant;
    invalidGeneration.resourceGeneration = 0;
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-grant.v1",
        &invalidGeneration,
        sizeof(invalidGeneration));
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceGrantInvalid,
        "a malformed resource generation must reject after transport");
    xpc_release(message);

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-payload.v1",
        changedBytes.data(),
        changedBytes.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadDigestMismatch,
        "same-sized mediated payload mutation must reject by digest");
    xpc_release(message);

    message = exactMessage();
    std::vector<std::uint8_t> oversized(
        activation::maximumComponentActivationImmutableResourceBytesV1 + 1U,
        0);
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-payload.v1",
        oversized.data(),
        oversized.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadSizeInvalid,
        "an over-budget resource payload must reject before digesting");
    xpc_release(message);

    message = exactMessage();
    auto wrongParameter = parameters;
    wrongParameter.front().text[0] = 'X';
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.parameters.v1",
        wrongParameter.data(),
        sizeof(wrongParameter));
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                StaticParametersDigestMismatch,
        "a resource parameter detached from authorization must reject");
    xpc_release(message);

    message = exactMessage();
    const std::uint8_t unexpected = 1;
    xpc_dictionary_set_data(message, "unexpected-resource-copy", &unexpected, 1);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid,
        "a duplicate or extra resource field must reject by exact count");
    xpc_release(message);

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.audio-stream-grant.v1",
        &unexpected,
        1);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid,
        "a static resource message must reject an injected Audio grant field");
    xpc_release(message);

    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0, "resource materialization file should close");
}

void testImmutableBinaryLoadPayloadIsExactAndFailClosed()
{
    auto sealed = sealedSession();
    const auto authority = resourceAuthorization(sealed);
    const auto parameters = resourceParameters();
    const auto bytes = binaryResourcePayload();
    const auto grant = binaryResourceGrant(authority, bytes);
    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    char fileTemplate[] = "/private/tmp/talas-activation-binary-envelope.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0, "Binary materialization file should allocate");
    require(unlink(fileTemplate) == 0,
        "Binary materialization file should unlink");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(materializationFile != nullptr,
        "Binary materialization XPC fd should allocate");

    const auto exactMessage = [&] {
        return activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            &grant,
            bytes);
    };
    auto message = exactMessage();
    require(message != nullptr,
        "an exact bounded Binary payload should enter the XPC envelope");
    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && decoded.resourceGrant.has_value()
            && std::memcmp(
                   &*decoded.resourceGrant, &grant, sizeof(grant)) == 0
            && std::ranges::equal(decoded.resourceBytes, bytes),
        "Binary XPC decode must retain the exact grant and raw octets");
    xpc_release(message);

    auto changedBytes = bytes;
    changedBytes[2] ^= 0xffU;
    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            &grant,
            changedBytes)
            == nullptr,
        "same-sized Binary mutation must reject before transport");

    const auto rejectsGrant = [&](auto mutate, const char* messageText) {
        auto changed = grant;
        mutate(changed);
        require(
            activation::createComponentActivationXpcLoadPayloadV1(
                authority,
                sealed,
                manifestBytes,
                entry,
                parameters,
                materializationFile,
                &changed,
                bytes)
                == nullptr,
            messageText);
    };
    rejectsGrant([](auto& value) { ++value.elementCount; },
        "Binary element-count drift must reject");
    rejectsGrant(
        [](auto& value) {
            value.resourceKind = static_cast<std::uint32_t>(
                activation::ComponentActivationImmutableResourceKindV1::
                    Table);
        },
        "Binary payload with Table kind must reject");
    rejectsGrant(
        [](auto& value) {
            value.elementFormat = static_cast<std::uint32_t>(
                activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
        },
        "Binary payload with Table format must reject");
    rejectsGrant(
        [](auto& value) { value.sourceContentDigest.front() ^= 0xffU; },
        "Binary source and mediated digest drift must reject");
    rejectsGrant(
        [](auto& value) { value.mediatedPayloadDigest.front() ^= 0xffU; },
        "Binary mediated digest drift must reject");

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-payload.v1",
        bytes.data(),
        bytes.size() - 1U);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadSizeInvalid,
        "a truncated Binary XPC payload must reject before copying");
    xpc_release(message);

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-payload.v1",
        changedBytes.data(),
        changedBytes.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadDigestMismatch,
        "a same-sized Binary XPC mutation must reject by exact digest");
    xpc_release(message);

    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0,
        "Binary materialization file should close");
}

void testAudioSourceLoadPayloadIsPathlessPrivateAndFailClosed()
{
    auto sealed = sealedSession();
    const auto authority = audioAuthorization(sealed);
    const auto parameters = audioParameters();
    const auto bytes = audioSourceBytes();
    const auto grant = audioStreamGrant(authority, bytes);
    require(
        activation::validateComponentActivationAudioStreamGrantV1(
            grant, authority, sealed, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "Audio stream grant fixture should be exact");
    auto audioMemory =
        activation::createComponentActivationXpcAudioSourceSharedMemoryV1(
            grant, bytes);
    require(audioMemory != nullptr,
        "exact Audio bytes should create anonymous XPC shared memory");

    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    char fileTemplate[] = "/private/tmp/talas-activation-audio-envelope.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0, "Audio materialization file should allocate");
    require(unlink(fileTemplate) == 0,
        "Audio materialization file should unlink immediately");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(materializationFile != nullptr,
        "Audio materialization XPC fd should allocate");

    const auto exactMessage = [&] {
        return activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            nullptr,
            {},
            &grant,
            audioMemory);
    };
    auto message = exactMessage();
    require(
        message != nullptr && xpc_dictionary_get_count(message) == 7,
        "Audio load must contain exactly seven typed pathless fields");
    require(
        xpc_dictionary_get_value(
            message,
            "talas.component-activation.immutable-resource-payload.v1")
            == nullptr,
        "Audio load must not reuse the static resource payload field");

    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && decoded.audioStreamGrant.has_value()
            && std::memcmp(
                   &*decoded.audioStreamGrant, &grant, sizeof(grant)) == 0
            && decoded.audioSourceSharedMemory != nullptr
            && xpc_get_type(decoded.audioSourceSharedMemory) == XPC_TYPE_SHMEM
            && !decoded.resourceGrant.has_value()
            && decoded.resourceBytes.empty(),
        "Audio envelope must decode only its exact grant and anonymous source");
    activation::ComponentActivationPrivateAudioSourceV1 privateSource;
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            decoded.audioSourceSharedMemory, grant, privateSource)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && privateSource.valid()
            && std::ranges::equal(privateSource.bytes(), bytes)
            && privateSource.mappedByteCount() >= bytes.size()
            && isReadOnlyNonExecutable(privateSource.bytes()),
        "Runtime must retain an exact independent private Audio copy");
    xpc_release(message);
    xpc_release(audioMemory);
    require(
        privateSource.valid() && std::ranges::equal(privateSource.bytes(), bytes),
        "private Audio bytes must outlive the incoming XPC memory entry");

    for (const auto frameCount : {1'013U, 1'014U})
    {
        const auto boundaryBytes = audioSourceBytes(frameCount);
        const auto boundaryGrant = audioStreamGrant(authority, boundaryBytes);
        auto boundaryMemory =
            activation::createComponentActivationXpcAudioSourceSharedMemoryV1(
                boundaryGrant, boundaryBytes);
        activation::ComponentActivationPrivateAudioSourceV1 boundarySource;
        require(
            boundaryMemory != nullptr
                && activation::materializeComponentActivationPrivateAudioSourceV1(
                    boundaryMemory, boundaryGrant, boundarySource)
                    == activation::
                        ComponentActivationXpcEnvelopeDiagnosticV1::Valid
                && std::ranges::equal(boundarySource.bytes(), boundaryBytes)
                && isReadOnlyNonExecutable(boundarySource.bytes()),
            "exact-page and page-plus-frame Audio sources must copy privately");
        xpc_release(boundaryMemory);
    }

    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            nullptr,
            {},
            &grant,
            nullptr)
            == nullptr,
        "Audio authorization without anonymous source memory must reject");

    audioMemory =
        activation::createComponentActivationXpcAudioSourceSharedMemoryV1(
            grant, bytes);
    require(audioMemory != nullptr,
        "Audio source memory should recreate for adversarial envelopes");
    auto changedGrant = grant;
    changedGrant.sourceContentDigest.front() ^= 0xffU;
    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            authority,
            sealed,
            manifestBytes,
            entry,
            parameters,
            materializationFile,
            nullptr,
            {},
            &changedGrant,
            audioMemory)
            == nullptr,
        "Audio grant digest drift must reject before transport");

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.audio-stream-grant.v1",
        &grant,
        sizeof(grant) - 1U);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioStreamGrantSizeInvalid,
        "a truncated Audio grant must reject before copying");
    xpc_release(message);

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.audio-source-shmem.v1",
        bytes.data(),
        bytes.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceInvalid,
        "an Audio data object must not impersonate anonymous shared memory");
    xpc_release(message);

    message = exactMessage();
    xpc_dictionary_set_data(
        message,
        "talas.component-activation.immutable-resource-payload.v1",
        bytes.data(),
        bytes.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid,
        "Audio plus static payload fields must reject by exact count");
    xpc_release(message);

    auto changedBytes = bytes;
    changedBytes.back() ^= 0xffU;
    auto adversarial = adversarialAudioSharedMemory(changedBytes);
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            adversarial, grant, privateSource)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceDigestMismatch,
        "same-sized Audio mutation must reject before private publication");
    xpc_release(adversarial);

    const auto pageBytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* mutableAfterSend = mmap(
        nullptr,
        pageBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_PRIVATE,
        -1,
        0);
    require(mutableAfterSend != MAP_FAILED,
        "post-send Audio mutation mapping should allocate");
    std::memcpy(mutableAfterSend, bytes.data(), bytes.size());
    adversarial = xpc_shmem_create(mutableAfterSend, pageBytes);
    require(adversarial != nullptr,
        "post-send Audio mutation XPC memory should allocate");
    static_cast<std::uint8_t*>(mutableAfterSend)[bytes.size() - 1U] ^= 0xffU;
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            adversarial, grant, privateSource)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceDigestMismatch,
        "receiver must reject source mutation after XPC memory entry creation");
    xpc_release(adversarial);
    require(munmap(mutableAfterSend, pageBytes) == 0,
        "post-send Audio mutation mapping should release");

    adversarial = adversarialAudioSharedMemory(bytes, 0, 0x7f);
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            adversarial, grant, privateSource)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceInvalid,
        "a nonzero Audio shared-memory page tail must reject");
    xpc_release(adversarial);

    adversarial = adversarialAudioSharedMemory(bytes, 1);
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            adversarial, grant, privateSource)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceSizeInvalid,
        "a noncanonical Audio shared-memory mapping size must reject");
    xpc_release(adversarial);

    auto malformedBytes = bytes;
    malformedBytes.front() = 'X';
    auto malformedGrant = grant;
    malformedGrant.sourceContentDigest =
        activation::calculateComponentActivationByteDigestV1(malformedBytes);
    adversarial = adversarialAudioSharedMemory(malformedBytes);
    require(
        activation::materializeComponentActivationPrivateAudioSourceV1(
            adversarial, malformedGrant, privateSource)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceMediaInvalid,
        "digest-matching malformed PCM16-WAV bytes must still reject");
    xpc_release(adversarial);

    xpc_release(audioMemory);
    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0,
        "Audio materialization file should close");
}

void testResourceSetPayloadIsAtomicOrderedAndPrivatelyMaterialized()
{
    ResourceSetEnvelopeFixture fixture;
    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    char fileTemplate[] = "/private/tmp/talas-activation-set-envelope.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0,
        "resource-set materialization file should allocate");
    require(unlink(fileTemplate) == 0,
        "resource-set materialization file should unlink");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(materializationFile != nullptr,
        "resource-set materialization XPC fd should allocate");

    auto exactSetObject = activation::createComponentActivationXpcResourceSetV1(
        fixture.authority,
        fixture.sealed,
        fixture.parameters,
        fixture.setGrant,
        fixture.members,
        fixture.sources);
    require(
        exactSetObject != nullptr
            && xpc_dictionary_get_count(exactSetObject) == 2,
        "the exact set should be one self-contained summary plus member array");
    xpc_release(exactSetObject);

    const auto exactMessage = [&] {
        return activation::createComponentActivationXpcLoadPayloadV1(
            fixture.authority,
            fixture.sealed,
            manifestBytes,
            entry,
            fixture.parameters,
            materializationFile,
            nullptr,
            {},
            nullptr,
            nullptr,
            &fixture.setGrant,
            fixture.members,
            fixture.sources);
    };
    auto message = exactMessage();
    require(
        message != nullptr && xpc_dictionary_get_count(message) == 6,
        "one exact resource set should use six top-level typed fields");
    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && decoded.resourceSetGrant.has_value()
            && decoded.resourceSetMembers.size() == fixture.members.size(),
        "the complete ordered resource set should decode atomically");
    activation::ComponentActivationPrivateResourceSetV1 privateSet;
    require(
        activation::materializeComponentActivationResourceSetV1(
            decoded, privateSet)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && privateSet.valid()
            && privateSet.memberCount() == fixture.members.size()
            && std::ranges::equal(privateSet.audioBytes(2), fixture.audioBytes)
            && isReadOnlyNonExecutable(privateSet.audioBytes(2))
            && privateSet.audioBytes(0).empty()
            && privateSet.audioBytes(1).empty(),
        "Runtime should publish only the complete set with private read-only Audio");
    xpc_release(message);

    const auto resourceSetObject = [](xpc_object_t value) {
        return xpc_dictionary_get_value(
            value, "talas.component-activation.resource-set.v1");
    };
    const auto resourceSetMemberArray = [&](xpc_object_t value) {
        return xpc_dictionary_get_value(
            resourceSetObject(value),
            "talas.component-activation.resource-set-members.v1");
    };

    std::size_t allocationFailures = 0;
    bool allocationSweepSucceeded = false;
    for (std::int64_t countdown = 0; countdown < 64; ++countdown)
    {
        message = exactMessage();
        require(message != nullptr,
            "allocation sweep should construct the set before injection");
        failAllocationAfter.store(countdown, std::memory_order_relaxed);
        const auto diagnostic =
            activation::decodeComponentActivationXpcLoadPayloadV1(
                message, fixture.sealed, decoded);
        failAllocationAfter.store(-1, std::memory_order_relaxed);
        xpc_release(message);
        if (diagnostic
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            allocationSweepSucceeded = true;
            break;
        }
        require(
            !decoded.resourceSetGrant.has_value()
                && decoded.resourceSetMembers.empty(),
            "every injected decode allocation failure must expose no partial set");
        ++allocationFailures;
    }
    require(
        allocationSweepSucceeded && allocationFailures >= 3,
        "the allocation sweep should fail every bounded set allocation then succeed");

    require(
        activation::createComponentActivationXpcLoadPayloadV1(
            fixture.authority,
            fixture.sealed,
            manifestBytes,
            entry,
            fixture.parameters,
            materializationFile)
            == nullptr,
        "a multiple-resource authority without the exact set must reject");

    message = exactMessage();
    auto members = resourceSetMemberArray(message);
    auto shortArray = xpc_array_create_empty();
    xpc_array_append_value(shortArray, xpc_array_get_value(members, 0));
    xpc_array_append_value(shortArray, xpc_array_get_value(members, 1));
    xpc_dictionary_set_value(
        resourceSetObject(message),
        "talas.component-activation.resource-set-members.v1",
        shortArray);
    xpc_release(shortArray);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMembersInvalid,
        "a missing resource-set member must reject before publication");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    auto reordered = xpc_array_create_empty();
    xpc_array_append_value(reordered, xpc_array_get_value(members, 1));
    xpc_array_append_value(reordered, xpc_array_get_value(members, 0));
    xpc_array_append_value(reordered, xpc_array_get_value(members, 2));
    xpc_dictionary_set_value(
        resourceSetObject(message),
        "talas.component-activation.resource-set-members.v1",
        reordered);
    xpc_release(reordered);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid,
        "a reordered resource-set member must reject by ordinal and digest");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    auto duplicated = xpc_array_create_empty();
    xpc_array_append_value(duplicated, xpc_array_get_value(members, 0));
    xpc_array_append_value(duplicated, xpc_array_get_value(members, 0));
    xpc_array_append_value(duplicated, xpc_array_get_value(members, 2));
    xpc_dictionary_set_value(
        resourceSetObject(message),
        "talas.component-activation.resource-set-members.v1",
        duplicated);
    xpc_release(duplicated);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid,
        "a duplicated resource-set member must reject atomically");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    auto first = xpc_array_get_value(members, 0);
    xpc_dictionary_set_data(
        first,
        "talas.component-activation.immutable-resource-grant.v1",
        &fixture.binaryGrant,
        sizeof(fixture.binaryGrant));
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid,
        "swapping same-shaped member grants must reject by exact pairing");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    first = xpc_array_get_value(members, 0);
    auto changedTable = fixture.tableBytes;
    changedTable.front() ^= 0x01U;
    xpc_dictionary_set_data(
        first,
        "talas.component-activation.immutable-resource-payload.v1",
        changedTable.data(),
        changedTable.size());
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid,
        "a same-sized set member mutation must reject by digest");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    auto audioMember = xpc_array_get_value(members, 2);
    auto nonzeroTail = adversarialAudioSharedMemory(
        fixture.audioBytes, 0, 0x7f);
    xpc_dictionary_set_value(
        audioMember,
        "talas.component-activation.audio-source-shmem.v1",
        nonzeroTail);
    xpc_release(nonzeroTail);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid,
        "a nonzero Audio page tail must reject the complete set");
    xpc_release(message);

    message = exactMessage();
    members = resourceSetMemberArray(message);
    auto wrongMembersType = xpc_data_create("x", 1);
    xpc_dictionary_set_value(
        resourceSetObject(message),
        "talas.component-activation.resource-set-members.v1",
        wrongMembersType);
    xpc_release(wrongMembersType);
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMembersInvalid,
        "an XPC data object must not impersonate the member array");
    xpc_release(message);

    message = exactMessage();
    require(
        activation::decodeComponentActivationXpcLoadPayloadV1(
            message, fixture.sealed, decoded)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid,
        "the exact set should decode again for partial-materialization proof");
    activation::ComponentActivationXpcLoadPayloadViewV1 partial;
    partial.resourceSetGrant = *decoded.resourceSetGrant;
    partial.resourceSetGrant->memberCount = 2;
    partial.resourceSetGrant->audioStreamCount = 2;
    partial.resourceSetMembers = {
        decoded.resourceSetMembers[2], decoded.resourceSetMembers[2]};
    auto changedAudio = fixture.audioBytes;
    changedAudio.back() ^= 0xffU;
    auto invalidSecond = adversarialAudioSharedMemory(changedAudio);
    partial.resourceSetMembers[1].audioSourceSharedMemory = invalidSecond;
    require(
        activation::materializeComponentActivationResourceSetV1(
            partial, privateSet)
            == activation::ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceDigestMismatch
            && !privateSet.valid() && privateSet.memberCount() == 0,
        "failure after one Audio copy must publish no partial private set");
    xpc_release(invalidSecond);
    xpc_release(message);

    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0,
        "resource-set materialization file should close");
}

void testImmutableOnlyResourceSetPayloadRoundTrips()
{
    ResourceSetEnvelopeFixture fixture;
    fixture.sealed.supportedPermissions = componentPermission(
            ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    fixture.authority.requestedPermissions =
        fixture.sealed.supportedPermissions;
    fixture.authority.declaredRoles = componentRole(
            ComponentExecutionRole::RealtimeProcessor)
        | componentRole(
            ComponentExecutionRole::ImmutableResourceConsumer);
    std::array<activation::ComponentActivationStaticParameterV1, 2>
        parameters{fixture.parameters[0], fixture.parameters[1]};
    fixture.authority.staticParameterCount = parameters.size();
    fixture.authority.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    std::array<activation::ComponentActivationResourceSetMemberV1, 2>
        members{fixture.members[0], fixture.members[1]};
    for (auto& member : members)
        member.memberCount = members.size();
    auto setGrant = fixture.setGrant;
    setGrant.memberCount = members.size();
    setGrant.audioStreamCount = 0;
    setGrant.aggregateSourceBytes = 0;
    setGrant.aggregateTransportBytes = 0;
    setGrant.aggregatePrivateMappedBytes = 0;
    setGrant.aggregateWorkingBytes = 0;
    for (const auto& member : members)
    {
        setGrant.aggregateSourceBytes += member.sourceByteCount;
        setGrant.aggregateTransportBytes += member.transportByteCount;
        setGrant.aggregatePrivateMappedBytes += member.privateMappedByteCount;
        setGrant.aggregateWorkingBytes += member.workingByteCount;
    }
    setGrant.resourceSetDigest =
        activation::calculateComponentActivationResourceSetDigestV1(
            setGrant, members);
    for (auto& member : members)
        member.resourceSetDigest = setGrant.resourceSetDigest;
    const std::array<activation::ComponentActivationXpcResourceSetSourceV1, 2>
        sources{fixture.sources[0], fixture.sources[1]};

    constexpr std::string_view manifest =
        R"({"format":"talas.component-bundle","version":1})";
    const auto manifestBytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(manifest.data()),
        manifest.size()};
    constexpr std::array<std::uint8_t, 3> entry{'a', 'b', 'c'};
    char fileTemplate[] = "/private/tmp/talas-activation-immutable-set.XXXXXX";
    const auto fileDescriptor = mkstemp(fileTemplate);
    require(fileDescriptor >= 0,
        "immutable-only set materialization file should allocate");
    require(unlink(fileTemplate) == 0,
        "immutable-only set materialization file should unlink");
    auto materializationFile = xpc_fd_create(fileDescriptor);
    require(materializationFile != nullptr,
        "immutable-only set materialization fd should become XPC data");

    auto message = activation::createComponentActivationXpcLoadPayloadV1(
        fixture.authority,
        fixture.sealed,
        manifestBytes,
        entry,
        parameters,
        materializationFile,
        nullptr,
        {},
        nullptr,
        nullptr,
        &setGrant,
        members,
        sources);
    activation::ComponentActivationXpcLoadPayloadViewV1 decoded;
    require(
        message != nullptr
            && activation::decodeComponentActivationXpcLoadPayloadV1(
                   message, fixture.sealed, decoded)
                == activation::ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            && decoded.resourceSetGrant.has_value()
            && decoded.resourceSetGrant->memberCount == members.size()
            && decoded.resourceSetGrant->audioStreamCount == 0
            && decoded.resourceSetMembers.size() == members.size(),
        "an exact Table+Binary set should round-trip without Audio permission");

    if (message != nullptr)
        xpc_release(message);
    xpc_release(materializationFile);
    require(close(fileDescriptor) == 0,
        "immutable-only set materialization file should close");
}
void testAuthenticatedCheckpointEnvelopes()
{
    using Operation = activation::ComponentActivationCheckpointOperationV1;
    using Status = activation::ComponentActivationCheckpointStatusV1;
    using Diagnostic = activation::ComponentActivationXpcEnvelopeDiagnosticV1;
    const auto authority = authorization(sealedSession());
    for (const auto operation : {Operation::Query, Operation::Begin, Operation::Restore, Operation::Commit})
    {
        activation::ComponentActivationCheckpointCommandV1 request;
        request.command = activation::makeComponentActivationCommandV1(authority,
            activation::ComponentActivationRecordKindV1::Checkpoint, 20);
        request.operation = operation;
        request.completedAudioSequence = operation == Operation::Query ? 0 : 7;
        request.checkpointId = operation == Operation::Query ? 0 : (operation == Operation::Begin ? 20 : 9);
        auto message = activation::createComponentActivationXpcCheckpointCommandV1(request, authority);
        require(message != nullptr, "exact checkpoint request encodes");
        activation::ComponentActivationCheckpointCommandV1 decoded;
        require(activation::decodeComponentActivationXpcCheckpointCommandV1(message, authority, decoded) == Diagnostic::Valid
            && std::memcmp(&request, &decoded, sizeof(request)) == 0, "checkpoint request round-trips exact identity and boundary");
        xpc_dictionary_set_bool(message, "unexpected", true);
        require(activation::decodeComponentActivationXpcCheckpointCommandV1(message, authority, decoded) == Diagnostic::FieldCountInvalid
            && decoded.command.sequence == 0, "extra checkpoint fields reject before publishing decoded state");
        xpc_release(message);
        for (unsigned raw = 0; raw <= 9; ++raw)
        {
            const auto status = static_cast<Status>(raw);
            const bool allowed = status == Status::Fatal
                || (operation == Operation::Query && (status == Status::Supported || status == Status::Unsupported))
                || (operation == Operation::Begin && (status == Status::Captured || status == Status::Unsupported
                    || status == Status::CaptureFailed || status == Status::InvalidBoundary))
                || (operation == Operation::Restore && (status == Status::Restored || status == Status::InvalidBoundary))
                || (operation == Operation::Commit && (status == Status::Committed || status == Status::InvalidBoundary));
            activation::ComponentActivationCheckpointReplyV1 reply{request, status, 0};
            message = xpc_dictionary_create_empty();
            require(activation::writeComponentActivationXpcCheckpointReplyV1(message, reply, authority) == allowed,
                "reply status must belong to its exact operation and Pending must stay local");
            // Raw serialization also exercises untrusted invalid reply bytes.
            xpc_dictionary_set_data(message, "talas.component-activation.checkpoint-reply.v1", &reply, sizeof(reply));
            activation::ComponentActivationCheckpointReplyV1 decodedReply;
            require((activation::decodeComponentActivationXpcCheckpointReplyV1(message, authority, decodedReply) == Diagnostic::Valid) == allowed,
                "decoder rejects operation/status mismatch even when bypassing the writer");
            if (allowed)
            {
                require(activation::matchesComponentActivationCheckpointReplyV1(decodedReply, request, authority),
                    "parent matches exact authenticated request echo");
                auto another = request;
                ++another.command.sequence;
                if (operation == Operation::Begin) ++another.checkpointId;
                require(activation::validateComponentActivationCheckpointCommandV1(another, authority)
                    && !activation::matchesComponentActivationCheckpointReplyV1(decodedReply, another, authority),
                    "structurally valid reply for a different control sequence cannot finish this request");
                if (operation != Operation::Query)
                {
                    another = request; ++another.completedAudioSequence;
                    require(!activation::matchesComponentActivationCheckpointReplyV1(decodedReply, another, authority),
                        "different completed audio boundary cannot acknowledge a checkpoint");
                }
            }
            else require(decodedReply.request.command.sequence == 0, "invalid reply leaves no decoded authority");
            xpc_release(message);
        }
        activation::ComponentActivationCheckpointReplyV1 reservedReply{request, Status::Fatal, 1};
        message = xpc_dictionary_create_empty();
        require(!activation::writeComponentActivationXpcCheckpointReplyV1(message, reservedReply, authority),
            "nonzero reply reservation must reject");
        xpc_dictionary_set_data(message, "talas.component-activation.checkpoint-reply.v1", &reservedReply, sizeof(reservedReply));
        activation::ComponentActivationCheckpointReplyV1 invalidReply;
        require(activation::decodeComponentActivationXpcCheckpointReplyV1(message, authority, invalidReply) == Diagnostic::ProtocolInvalid
            && invalidReply.request.command.sequence == 0, "raw reserved reply cannot publish decoded authority");
        xpc_release(message);
        for (unsigned corruption = 0; corruption < 22; ++corruption)
        {
            auto bad = request;
            switch (corruption)
            {
                case 0: bad.command.header.magic ^= 1; break;
                case 1: ++bad.command.header.protocolVersion; break;
                case 2: bad.command.header.recordKind = static_cast<std::uint16_t>(activation::ComponentActivationRecordKindV1::Heartbeat); break;
                case 3: ++bad.command.header.recordBytes; break;
                case 4: bad.command.reserved0[0] = 1; break;
                case 5: bad.command.contentDigest[0] ^= 1; break;
                case 6: ++bad.command.artifactGeneration; break;
                case 7: ++bad.command.activationGeneration; break;
                case 8: ++bad.command.runtimeSessionGeneration; break;
                case 9: ++bad.command.sharedStorageIdentity; break;
                case 10: bad.command.validationNonce[0] ^= 1; break;
                case 11: bad.command.runtimeSessionNonce[0] ^= 1; break;
                case 12: bad.command.authorizationNonce[0] ^= 1; break;
                case 13: bad.command.sequence = 0; break;
                case 14: bad.command.sequence = std::numeric_limits<std::uint64_t>::max(); break;
                case 15: bad.operation = static_cast<Operation>(0); break;
                case 16: bad.operation = static_cast<Operation>(5); break;
                case 17: bad.completedAudioSequence = std::numeric_limits<std::uint64_t>::max(); break;
                case 18: bad.checkpointId = 21; break;
                case 19: bad.reserved0 = 1; break;
                case 20: bad.command.reserved0[1] = 1; break;
                case 21: bad.command.header.reserved0 = 1; break;
            }
            require(!activation::validateComponentActivationCheckpointCommandV1(bad, authority)
                && activation::createComponentActivationXpcCheckpointCommandV1(bad, authority) == nullptr,
                "checkpoint writer rejects corrupted authorization, operation or identity");
            message = xpc_dictionary_create_empty();
            xpc_dictionary_set_data(message, "talas.component-activation.checkpoint-command.v1", &bad, sizeof(bad));
            require(activation::decodeComponentActivationXpcCheckpointCommandV1(message, authority, decoded) == Diagnostic::ProtocolInvalid
                && decoded.command.sequence == 0, "untrusted corrupted checkpoint payload rejects before use");
            xpc_release(message);
        }
        std::array<std::byte, sizeof(request) + 1> raw{};
        std::memcpy(raw.data(), &request, sizeof(request));
        for (const auto bytes : {sizeof(request) - 1, sizeof(request) + 1})
        {
            message = xpc_dictionary_create_empty();
            xpc_dictionary_set_data(message, "talas.component-activation.checkpoint-command.v1", raw.data(), bytes);
            require(activation::decodeComponentActivationXpcCheckpointCommandV1(message, authority, decoded) == Diagnostic::CommandSizeInvalid,
                "truncated or extended checkpoint record rejects");
            xpc_release(message);
        }
        message = xpc_dictionary_create_empty();
        xpc_dictionary_set_bool(message, "talas.component-activation.checkpoint-command.v1", true);
        require(activation::decodeComponentActivationXpcCheckpointCommandV1(message, authority, decoded) == Diagnostic::CommandMissing,
            "checkpoint dictionary value type must be exact data");
        xpc_release(message);
    }
}
}

int main()
{
    testAuthenticatedCheckpointEnvelopes();
    testExactControlRecordsRoundTripWithoutComponentBytes();
    testMalformedAndPreauthorizationPayloadEnvelopesReject();
    testExactAuthorizedLoadPayloadIsDigestAndCountBound();
    testSealedPrewarmCarriesOnlyExactSharedStorage();
    testImmutableResourceLoadPayloadIsExactAndFailClosed();
    testImmutableBinaryLoadPayloadIsExactAndFailClosed();
    testAudioSourceLoadPayloadIsPathlessPrivateAndFailClosed();
    testResourceSetPayloadIsAtomicOrderedAndPrivatelyMaterialized();
    testImmutableOnlyResourceSetPayloadRoundTrips();
    return 0;
}
