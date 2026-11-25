#include "talas/runtime/KernelAbi.h"

#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <thread>

#include <unistd.h>

// Compiled once per integration group or fixture mode; helpers that the
// current configuration does not use are expected.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE
#define TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION
#define TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION "1.0.0"
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
#define TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_PROBE_COMPATIBLE
#define TALAS_COMPONENT_ACTIVATION_PROBE_COMPATIBLE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE >= 35 && \
    TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE <= 38
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE 1
#else
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 37 || \
    TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 38
#define TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE 1
#else
#define TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 36 || \
    TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 38
#define TALAS_COMPONENT_ACTIVATION_LATE_RESOURCE_RESOLVER_FIXTURE 1
#else
#define TALAS_COMPONENT_ACTIVATION_LATE_RESOURCE_RESOLVER_FIXTURE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE >= 39 && \
    TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE <= 51
#define TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE 1
#else
#define TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE >= 52 && \
    TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE <= 61
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE 1
#else
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE 0
#endif

// Original Talas Apache-2.0 test module. It is built only under
// TALAS_BUILD_TESTS and is never packaged into Workbench or a product helper.

namespace
{
[[noreturn]] void hangForever()
{
    while (true)
        static_cast<void>(pause());
}

#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
enum class ResourceSetFixtureKind : std::uint8_t
{
    Table,
    Binary,
    Audio,
};

struct ResourceSetFixtureMember
{
    std::string_view parameterId;
    std::string_view resourceId;
    ResourceSetFixtureKind kind;
};

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 56
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
};
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 57
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
};
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 58
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "binary", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
    ResourceSetFixtureMember{
        "audio-b", "Audio/Voice-B", ResourceSetFixtureKind::Audio},
};
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 59
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table-a", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary-a", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "table-b", "Tables/Curve-B", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary-b", "Binary/Blob-B", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "table-c", "Tables/Curve-C", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary-c", "Binary/Blob-C", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
    ResourceSetFixtureMember{
        "audio-b", "Audio/Voice-B", ResourceSetFixtureKind::Audio},
};
#else
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
    ResourceSetFixtureMember{
        "audio-b", "Audio/Voice-B", ResourceSetFixtureKind::Audio},
};
#endif
#endif

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 17
__attribute__((constructor)) void crashInConstructor()
{
    std::abort();
}
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 18
__attribute__((constructor)) void hangInConstructor()
{
    hangForever();
}
#endif

struct GainInstance
{
    const TalasKernelAbiHostServicesV1* host = nullptr;
    double gain = 1.0;
    std::uint64_t maximumFrames = 0;
    std::uint32_t resetCalls = 0;
    std::uint32_t processCalls = 0;
    double destinationSampleRate = 0.0;
    const std::uint8_t* resourceBytes = nullptr;
    std::uint64_t resourceByteCount = 0;
    std::uint64_t resourceElementCount = 0;
    std::array<TalasKernelAbiImmutableResourceViewV2, 6> resourceSetViews{};
    TalasKernelAbiImmutableAudioStreamViewV3 audioStream{};
    std::array<TalasKernelAbiImmutableAudioStreamViewV3, 2>
        resourceSetAudioStreams{};
    std::array<double, 1024> audioScratch{};
    std::array<std::array<double, 1024>, 2> resourceSetAudioScratch{};
    std::size_t resourceSetViewCount = 0;
    std::size_t resourceSetAudioStreamCount = 0;
    bool prepared = false;
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
    std::array<double, 2> checkpointHistory{};
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 7
    bool checkpointCaptureFailedOnce = false;
#endif
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 8
    std::uint32_t checkpointCaptureOrdinal = 0;
#endif
    bool checkpointHeld = false;
#endif
};

std::uint32_t destroyCount = 0;
std::uint32_t unloadCount = 0;

TalasKernelAbiStatusV1 create(
    const TalasKernelAbiHostServicesV1* host,
    void** instance)
{
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 7
    return TALAS_KERNEL_ABI_STATUS_FATAL;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 8
    throw 8;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 9
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 10
    hangForever();
#endif
    if (host == nullptr || host->allocate == nullptr
        || instance == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    }
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
    const auto* hostV4 = reinterpret_cast<const TalasKernelAbiHostServicesV4*>(host);
    if (host->structSize != sizeof(*hostV4)
        || host->servicesVersion != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4
        || hostV4->v3.v2.resolveImmutableResource == nullptr
        || hostV4->v3.resolveImmutableAudioStream == nullptr
        || hostV4->v3.readImmutableAudioStream == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    }
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE
    const auto* hostV2 = reinterpret_cast<const TalasKernelAbiHostServicesV2*>(host);
    if (host->structSize != sizeof(*hostV2)
        || host->servicesVersion != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V2
        || hostV2->resolveImmutableResource == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    }
#elif TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
    const auto* hostV3 = reinterpret_cast<
        const TalasKernelAbiHostServicesV3*>(host);
    if (host->structSize != sizeof(*hostV3)
        || host->servicesVersion != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3
        || hostV3->resolveImmutableAudioStream == nullptr
        || hostV3->readImmutableAudioStream == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    }
#endif
    auto* storage = host->allocate(
        host->hostContext, sizeof(GainInstance), alignof(GainInstance));
    if (storage == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    *instance = new (storage) GainInstance{host};
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 11
    for (std::size_t index = 0; index < 65; ++index)
    {
        static_cast<void>(host->allocate(
            host->hostContext, 1, alignof(std::max_align_t)));
    }
#endif
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 prepare(
    void* rawInstance,
    const TalasKernelAbiPrepareV1* value)
{
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 12
    return TALAS_KERNEL_ABI_STATUS_FATAL;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 13
    throw 13;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 14
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 15
    hangForever();
#endif
    auto* instance = static_cast<GainInstance*>(rawInstance);
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
    constexpr auto expectedParameterCount = resourceSetFixtureMembers.size();
#else
    constexpr auto expectedParameterCount = std::size_t{1};
#endif
    if (instance == nullptr || value == nullptr
        || value->structSize != sizeof(*value)
        || value->apiVersion
            != (TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
                    ? TALAS_KERNEL_API_VERSION_V4
                    : (TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
                        ? TALAS_KERNEL_API_VERSION_V3
                        : (TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE
                            ? TALAS_KERNEL_API_VERSION_V2
                            : TALAS_KERNEL_API_VERSION_V1)))
        || (value->sampleRate != 44'100.0 && value->sampleRate != 48'000.0)
        || (value->maximumBlockFrames != 256
            && value->maximumBlockFrames != 512)
        || value->parameterCount != expectedParameterCount
        || value->parameters == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    }
    const auto& parameter = value->parameters[0];
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
    if (value->host == nullptr
        || value->host->structSize != sizeof(TalasKernelAbiHostServicesV4)
        || value->host->servicesVersion
            != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
    for (std::size_t index = 0;
         index < resourceSetFixtureMembers.size();
         ++index)
    {
        const auto& definition = resourceSetFixtureMembers[index];
        const auto& candidate = value->parameters[index];
        if (candidate.structSize != sizeof(candidate)
            || candidate.kind != TALAS_KERNEL_ABI_PARAMETER_RESOURCE
            || candidate.id.data == nullptr
            || std::string_view{candidate.id.data, candidate.id.byteCount}
                != definition.parameterId
            || candidate.textValue.data == nullptr
            || std::string_view{
                   candidate.textValue.data, candidate.textValue.byteCount}
                != definition.resourceId
            || candidate.scalarValue != 0.0 || candidate.integerValue != 0
            || candidate.reserved0 != 0)
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
    }
    const auto* hostV4 = reinterpret_cast<
        const TalasKernelAbiHostServicesV4*>(value->host);
    std::size_t immutableIndex = 0;
    std::size_t audioIndex = 0;
    for (std::size_t memberIndex = 0;
         memberIndex < resourceSetFixtureMembers.size();
         ++memberIndex)
    {
        const auto& definition = resourceSetFixtureMembers[memberIndex];
        if (definition.kind != ResourceSetFixtureKind::Audio)
        {
            if (immutableIndex >= instance->resourceSetViews.size())
                return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
            auto& resource = instance->resourceSetViews[immutableIndex];
            const auto table =
                definition.kind == ResourceSetFixtureKind::Table;
            if (hostV4->v3.v2.resolveImmutableResource(
                    value->host->hostContext,
                    value->parameters[memberIndex].textValue,
                    &resource) != TALAS_KERNEL_ABI_STATUS_OK
                || resource.structSize != sizeof(resource)
                || resource.kind
                    != (table
                            ? TALAS_KERNEL_ABI_RESOURCE_TABLE
                            : TALAS_KERNEL_ABI_RESOURCE_BINARY)
                || resource.elementFormat
                    != (table
                            ? TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN
                            : TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES)
                || resource.id.data == nullptr
                || std::string_view{resource.id.data, resource.id.byteCount}
                    != definition.resourceId
                || resource.generation != 7U + memberIndex
                || resource.bytes.data == nullptr
                || resource.bytes.byteCount == 0U)
            {
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            ++immutableIndex;
            continue;
        }
        if (audioIndex >= instance->resourceSetAudioStreams.size())
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 55
        if (audioIndex == 1U)
        {
            ++audioIndex;
            continue;
        }
#endif
        TalasKernelAbiImmutableAudioStreamPrepareV3 streamPrepare{};
        streamPrepare.structSize = sizeof(streamPrepare);
        streamPrepare.apiVersion = TALAS_KERNEL_API_VERSION_V3;
        streamPrepare.id = value->parameters[memberIndex].textValue;
        streamPrepare.initialSourceFrame = 0.0;
        streamPrepare.maximumSourceFrameIncrement = 2.0;
        streamPrepare.maximumReadFrames = 512U;
        auto& stream = instance->resourceSetAudioStreams[audioIndex];
        if (hostV4->v3.resolveImmutableAudioStream(
                value->host->hostContext, &streamPrepare, &stream)
                != TALAS_KERNEL_ABI_STATUS_OK
            || stream.structSize != sizeof(stream)
            || stream.apiVersion != TALAS_KERNEL_API_VERSION_V3
            || stream.id.data == nullptr
            || std::string_view{stream.id.data, stream.id.byteCount}
                != definition.resourceId
            || stream.generation != 7U + memberIndex
            || stream.streamToken == 0U
            || stream.sampleRate != 48'000U
            || stream.frameCount == 0U
            || stream.maximumSourceFrameIncrement != 2.0
            || stream.maximumReadFrames != 512U)
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        ++audioIndex;
    }
    instance->resourceSetViewCount = immutableIndex;
    instance->resourceSetAudioStreamCount = audioIndex;
#elif TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Voice-A";
    if (parameter.structSize != sizeof(parameter)
        || parameter.kind != TALAS_KERNEL_ABI_PARAMETER_RESOURCE
        || parameter.id.data == nullptr
        || std::string_view{parameter.id.data, parameter.id.byteCount}
            != parameterId
        || parameter.textValue.data == nullptr
        || std::string_view{
               parameter.textValue.data, parameter.textValue.byteCount}
            != resourceId
        || parameter.scalarValue != 0.0 || parameter.integerValue != 0
        || parameter.reserved0 != 0 || value->host == nullptr
        || value->host->structSize != sizeof(TalasKernelAbiHostServicesV3)
        || value->host->servicesVersion
            != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
    const auto* hostV3 = reinterpret_cast<
        const TalasKernelAbiHostServicesV3*>(value->host);
    TalasKernelAbiImmutableAudioStreamPrepareV3 streamPrepare{};
    streamPrepare.structSize = sizeof(streamPrepare);
    streamPrepare.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    streamPrepare.id = parameter.textValue;
    streamPrepare.initialSourceFrame = 0.0;
    streamPrepare.maximumSourceFrameIncrement = 2.0;
    streamPrepare.maximumReadFrames = 512;
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 43
    TalasKernelAbiStatusV1 otherThreadStatus = TALAS_KERNEL_ABI_STATUS_OK;
    std::thread otherThread([&] {
        TalasKernelAbiImmutableAudioStreamViewV3 otherView{};
        otherThreadStatus = hostV3->resolveImmutableAudioStream(
            value->host->hostContext, &streamPrepare, &otherView);
    });
    otherThread.join();
    if (otherThreadStatus == TALAS_KERNEL_ABI_STATUS_OK)
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    instance->prepared = true;
    return TALAS_KERNEL_ABI_STATUS_OK;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 45
    instance->prepared = true;
    return TALAS_KERNEL_ABI_STATUS_OK;
#endif
    const auto resolved = hostV3->resolveImmutableAudioStream(
        value->host->hostContext,
        &streamPrepare,
        &instance->audioStream);
    if (resolved != TALAS_KERNEL_ABI_STATUS_OK
        || instance->audioStream.structSize
            != sizeof(instance->audioStream)
        || instance->audioStream.apiVersion != TALAS_KERNEL_API_VERSION_V3
        || instance->audioStream.id.data == nullptr
        || std::string_view{
               instance->audioStream.id.data,
               instance->audioStream.id.byteCount} != resourceId
        || instance->audioStream.generation != 7U
        || (instance->audioStream.channelCount != 1U
            && instance->audioStream.channelCount != 2U)
        || instance->audioStream.sampleRate != 48'000U
        || instance->audioStream.frameCount == 0U
        || instance->audioStream.streamToken == 0U
        || instance->audioStream.maximumSourceFrameIncrement != 2.0
        || instance->audioStream.maximumReadFrames != 512U)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 44
    TalasKernelAbiImmutableAudioStreamViewV3 duplicate{};
    if (hostV3->resolveImmutableAudioStream(
            value->host->hostContext, &streamPrepare, &duplicate)
        == TALAS_KERNEL_ABI_STATUS_OK)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE
    constexpr std::string_view parameterId = "table";
    constexpr std::string_view resourceId = "Tables/Curve-A";
    if (parameter.structSize != sizeof(parameter)
        || parameter.kind != TALAS_KERNEL_ABI_PARAMETER_RESOURCE
        || parameter.id.data == nullptr
        || std::string_view{parameter.id.data, parameter.id.byteCount}
            != parameterId
        || parameter.textValue.data == nullptr
        || std::string_view{
               parameter.textValue.data, parameter.textValue.byteCount}
            != resourceId
        || parameter.scalarValue != 0.0 || parameter.integerValue != 0
        || parameter.reserved0 != 0 || value->host == nullptr
        || value->host->structSize != sizeof(TalasKernelAbiHostServicesV2)
        || value->host->servicesVersion
            != TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V2)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
    const auto* hostV2 = reinterpret_cast<
        const TalasKernelAbiHostServicesV2*>(value->host);
    TalasKernelAbiImmutableResourceViewV2 resource{};
    const auto resolved = hostV2->resolveImmutableResource(
        value->host->hostContext, parameter.textValue, &resource);
    if (resolved != TALAS_KERNEL_ABI_STATUS_OK
        || resource.structSize != sizeof(resource)
        || resource.kind
            != (TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
                    ? TALAS_KERNEL_ABI_RESOURCE_BINARY
                    : TALAS_KERNEL_ABI_RESOURCE_TABLE)
        || resource.elementFormat
            != (TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
                    ? TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES
                    : TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN)
        || resource.id.data == nullptr
        || std::string_view{resource.id.data, resource.id.byteCount}
            != resourceId
        || resource.generation != 7 || resource.bytes.data == nullptr
        || resource.elementCount
            != (TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE ? 5U : 3U)
        || resource.bytes.byteCount
            != (TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
                    ? resource.elementCount
                    : resource.elementCount * sizeof(double))
        || resource.reserved0 != 0 || resource.reserved1[0] != 0
        || resource.reserved1[1] != 0)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
#if TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
    if (std::memcmp(
            resource.sourceContentDigest,
            resource.mediatedPayloadDigest,
            TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2)
            != 0
        || resource.bytes.data[0] != 0x00
        || resource.bytes.data[1] != 0x7f
        || resource.bytes.data[2] != 0x80
        || resource.bytes.data[3] != 0xfe
        || resource.bytes.data[4] != 0xff)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
#endif
    instance->resourceBytes = resource.bytes.data;
    instance->resourceByteCount = resource.bytes.byteCount;
    instance->resourceElementCount = resource.elementCount;
#else
    constexpr std::string_view gainId = "gain";
    if (parameter.structSize != sizeof(parameter)
        || parameter.kind != TALAS_KERNEL_ABI_PARAMETER_SCALAR
        || parameter.id.data == nullptr
        || std::string_view{parameter.id.data, parameter.id.byteCount} != gainId
        || !std::isfinite(parameter.scalarValue)
        || parameter.integerValue != 0
        || parameter.textValue.byteCount != 0
        || parameter.reserved0 != 0)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
    instance->gain = parameter.scalarValue;
#endif
    instance->maximumFrames = value->maximumBlockFrames;
    instance->destinationSampleRate = value->sampleRate;
    instance->prepared = true;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 reset(
    void* rawInstance,
    const TalasKernelAbiResetV1* value)
{
    auto* instance = static_cast<GainInstance*>(rawInstance);
    if (instance == nullptr || !instance->prepared || value == nullptr
        || value->structSize != sizeof(*value))
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    }
    const auto call = ++instance->resetCalls;
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
    instance->checkpointHistory.fill(0);
#endif
    if (call >= 2)
    {
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 25
        return TALAS_KERNEL_ABI_STATUS_FATAL;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 26
        throw 26;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 27
        std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 28
        hangForever();
#endif
    }
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 42
    const auto* hostV3 = reinterpret_cast<
        const TalasKernelAbiHostServicesV3*>(instance->host);
    TalasKernelAbiImmutableAudioStreamReadV3 read{};
    read.structSize = sizeof(read);
    read.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    read.streamToken = instance->audioStream.streamToken;
    read.absoluteSourceFrame = 0.0;
    read.sourceFrameIncrement = 1.0;
    read.frameCount = 1U;
    read.outputChannelCount = instance->audioStream.channelCount;
    read.interleavedOutput = instance->audioScratch.data();
    read.outputSampleCapacity = instance->audioStream.channelCount;
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    if (hostV3->readImmutableAudioStream(
            instance->host->hostContext, &read, &result)
        == TALAS_KERNEL_ABI_STATUS_OK)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 process(
    void* rawInstance,
    const TalasKernelAbiProcessV1* value)
{
    auto* instance = static_cast<GainInstance*>(rawInstance);
    if (instance == nullptr || !instance->prepared || value == nullptr
        || value->structSize != sizeof(*value)
        || value->frameCount == 0
        || value->frameCount > instance->maximumFrames
        || value->inputCount == 0
        || value->inputCount != value->outputCount
        || value->inputs == nullptr || value->outputs == nullptr)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    }
    const auto call = ++instance->processCalls;
#if defined(TALAS_COMPONENT_ACTIVATION_DELAY_FIRST_PROCESS)
    if (call == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
    if (call >= 2)
    {
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 29
        return TALAS_KERNEL_ABI_STATUS_FATAL;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 30
        throw 30;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 31
        std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 32
        hangForever();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 34
        return static_cast<TalasKernelAbiStatusV1>(UINT32_MAX);
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 46
        std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 47
        hangForever();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 60
        std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 61
        hangForever();
#endif
    }
#if TALAS_COMPONENT_ACTIVATION_LATE_RESOURCE_RESOLVER_FIXTURE
    const auto* hostV2 = reinterpret_cast<
        const TalasKernelAbiHostServicesV2*>(instance->host);
    TalasKernelAbiImmutableResourceViewV2 lateResource{};
    constexpr TalasKernelAbiStringViewV1 resourceId{
        "Tables/Curve-A", sizeof("Tables/Curve-A") - 1U};
    if (hostV2 == nullptr || hostV2->resolveImmutableResource == nullptr
        || hostV2->resolveImmutableResource(
               instance->host->hostContext, resourceId, &lateResource)
            != TALAS_KERNEL_ABI_STATUS_NOT_PREPARED)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
    const auto* hostV4 = reinterpret_cast<
        const TalasKernelAbiHostServicesV4*>(instance->host);
    for (std::size_t index = 0;
         index < instance->resourceSetAudioStreamCount;
         ++index)
    {
        const auto& stream = instance->resourceSetAudioStreams[index];
        TalasKernelAbiImmutableAudioStreamReadV3 streamRead{};
        streamRead.structSize = sizeof(streamRead);
        streamRead.apiVersion = TALAS_KERNEL_API_VERSION_V3;
        streamRead.streamToken = stream.streamToken;
        const auto increment = static_cast<double>(stream.sampleRate)
            / instance->destinationSampleRate;
        streamRead.absoluteSourceFrame = static_cast<double>(
            value->transport.absoluteFrame) * increment;
        streamRead.sourceFrameIncrement = increment;
        streamRead.frameCount = value->frameCount;
        streamRead.outputChannelCount = stream.channelCount;
        streamRead.interleavedOutput =
            instance->resourceSetAudioScratch[index].data();
        streamRead.outputSampleCapacity =
            value->frameCount * stream.channelCount;
        TalasKernelAbiImmutableAudioStreamReadResultV3 streamResult{};
        if (hostV4->v3.readImmutableAudioStream(
                instance->host->hostContext, &streamRead, &streamResult)
                != TALAS_KERNEL_ABI_STATUS_OK
            || streamResult.structSize != sizeof(streamResult)
            || streamResult.frameCount != value->frameCount)
        {
            return TALAS_KERNEL_ABI_STATUS_FATAL;
        }
        if (TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 53 && index == 1U)
        {
            TalasKernelAbiImmutableAudioStreamReadResultV3 repeated{};
            auto repeatedRead = streamRead;
            repeatedRead.streamToken =
                instance->resourceSetAudioStreams[0].streamToken;
            if (hostV4->v3.readImmutableAudioStream(
                    instance->host->hostContext, &repeatedRead, &repeated)
                == TALAS_KERNEL_ABI_STATUS_OK)
            {
                return TALAS_KERNEL_ABI_STATUS_FATAL;
            }
        }
        if (TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 54 && index == 1U)
        {
            TalasKernelAbiImmutableAudioStreamReadResultV3 invalid{};
            auto invalidRead = streamRead;
            invalidRead.streamToken ^= UINT64_C(0x8000000000000000);
            if (hostV4->v3.readImmutableAudioStream(
                    instance->host->hostContext, &invalidRead, &invalid)
                == TALAS_KERNEL_ABI_STATUS_OK)
            {
                return TALAS_KERNEL_ABI_STATUS_FATAL;
            }
        }
    }
#elif TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
    const auto* hostV3 = reinterpret_cast<
        const TalasKernelAbiHostServicesV3*>(instance->host);
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 40
    TalasKernelAbiImmutableAudioStreamPrepareV3 latePrepare{};
    latePrepare.structSize = sizeof(latePrepare);
    latePrepare.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    latePrepare.id = instance->audioStream.id;
    latePrepare.initialSourceFrame = 0.0;
    latePrepare.maximumSourceFrameIncrement = 2.0;
    latePrepare.maximumReadFrames = 512;
    TalasKernelAbiImmutableAudioStreamViewV3 lateView{};
    if (hostV3->resolveImmutableAudioStream(
            instance->host->hostContext, &latePrepare, &lateView)
        == TALAS_KERNEL_ABI_STATUS_OK)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
    TalasKernelAbiImmutableAudioStreamReadV3 streamRead{};
    streamRead.structSize = sizeof(streamRead);
    streamRead.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    streamRead.streamToken = instance->audioStream.streamToken;
    const auto sourceFrameIncrement =
        static_cast<double>(instance->audioStream.sampleRate)
        / instance->destinationSampleRate;
    streamRead.absoluteSourceFrame = static_cast<double>(
        value->transport.absoluteFrame) * sourceFrameIncrement;
    streamRead.sourceFrameIncrement = sourceFrameIncrement;
    const auto streamFrameCount =
        TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 49
        ? 1U
        : (TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 50
            ? 7U
            : (TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 51 && call >= 2
                ? 513U
                : value->frameCount));
    streamRead.frameCount = streamFrameCount;
    streamRead.outputChannelCount = instance->audioStream.channelCount;
    streamRead.interleavedOutput = instance->audioScratch.data();
    streamRead.outputSampleCapacity = streamFrameCount
        * instance->audioStream.channelCount;
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 48
    if (call >= 2)
        streamRead.interleavedOutput = nullptr;
#endif
    TalasKernelAbiImmutableAudioStreamReadResultV3 streamResult{};
    if (hostV3->readImmutableAudioStream(
            instance->host->hostContext, &streamRead, &streamResult)
            != TALAS_KERNEL_ABI_STATUS_OK
        || streamResult.structSize != sizeof(streamResult)
        || streamResult.frameCount != streamFrameCount)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 41
    TalasKernelAbiImmutableAudioStreamReadResultV3 repeated{};
    if (hostV3->readImmutableAudioStream(
            instance->host->hostContext, &streamRead, &repeated)
        == TALAS_KERNEL_ABI_STATUS_OK)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
#endif
    for (std::uint64_t channel = 0; channel < value->inputCount; ++channel)
    {
        const auto& input = value->inputs[channel];
        const auto& output = value->outputs[channel];
        if (input.structSize != sizeof(input)
            || output.structSize != sizeof(output)
            || output.samples == nullptr
            || output.sampleCount != value->frameCount)
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
        }
        for (std::uint64_t frame = 0; frame < value->frameCount; ++frame)
        {
            const auto sample = input.kind == TALAS_KERNEL_ABI_SIGNAL_CONSTANT
                ? input.constantValue
                : input.samples[frame];
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE <= 55
            const auto channelsA =
                instance->resourceSetAudioStreams[0].channelCount;
            const auto channelsB =
                instance->resourceSetAudioStreams[1].channelCount;
            const auto audioA = instance->resourceSetAudioScratch[0][
                frame * channelsA + channel];
            const auto audioB = instance->resourceSetAudioScratch[1][
                frame * channelsB + channel];
            double tableValue = 0.0;
            const auto& table = instance->resourceSetViews[0];
            const auto tableIndex = frame % table.elementCount;
            std::memcpy(
                &tableValue,
                table.bytes.data + tableIndex * sizeof(double),
                sizeof(tableValue));
            const auto& binary = instance->resourceSetViews[1];
            const auto binaryScale = static_cast<double>(
                binary.bytes.data[frame % binary.bytes.byteCount]) / 255.0;
            output.samples[frame] = sample
                * (audioA * tableValue + audioB * binaryScale);
#else
            double resourceValue = 0.0;
            std::size_t immutableIndex = 0;
            std::size_t audioIndex = 0;
            for (const auto& definition : resourceSetFixtureMembers)
            {
                if (definition.kind == ResourceSetFixtureKind::Audio)
                {
                    const auto channels = instance
                        ->resourceSetAudioStreams[audioIndex]
                        .channelCount;
                    resourceValue += instance->resourceSetAudioScratch[
                        audioIndex][frame * channels + channel];
                    ++audioIndex;
                    continue;
                }
                const auto& resource =
                    instance->resourceSetViews[immutableIndex++];
                if (definition.kind == ResourceSetFixtureKind::Table)
                {
                    double tableValue = 0.0;
                    const auto tableIndex = frame % resource.elementCount;
                    std::memcpy(
                        &tableValue,
                        resource.bytes.data
                            + tableIndex * sizeof(double),
                        sizeof(tableValue));
                    resourceValue += tableValue;
                }
                else
                {
                    resourceValue += static_cast<double>(
                        resource.bytes.data[
                            frame % resource.bytes.byteCount]) / 255.0;
                }
            }
            output.samples[frame] = sample * resourceValue;
#endif
#elif TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
            output.samples[frame] = instance->audioScratch[
                frame * instance->audioStream.channelCount + channel];
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE
            if (instance->resourceBytes == nullptr
                || instance->resourceElementCount == 0
                || instance->resourceByteCount
                    != instance->resourceElementCount
                        * (TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
                                ? 1U
                                : sizeof(double)))
            {
                return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
            }
#if TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE
            const auto byteIndex = frame % instance->resourceElementCount;
            output.samples[frame] = sample
                * static_cast<double>(instance->resourceBytes[byteIndex]);
#else
            double tableValue = 0.0;
            const auto tableIndex = frame % instance->resourceElementCount;
            std::memcpy(
                &tableValue,
                instance->resourceBytes + tableIndex * sizeof(double),
                sizeof(tableValue));
            output.samples[frame] = sample * tableValue;
#endif
#else
            output.samples[frame] = sample * instance->gain;
#endif
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
            auto& history = instance->checkpointHistory[channel];
            history = history * 0.5 + output.samples[frame];
            output.samples[frame] = history;
#endif
        }
    }
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 33
    if (call >= 2)
        value->outputs[0].samples[0] = std::numeric_limits<double>::quiet_NaN();
#endif
    return TALAS_KERNEL_ABI_STATUS_OK;
}

void destroy(
    void* rawInstance,
    const TalasKernelAbiHostServicesV1* host)
{
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
    if (static_cast<GainInstance*>(rawInstance)->checkpointHeld) std::abort();
#endif
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 19
    ++destroyCount;
    throw 19;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 20
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 21
    hangForever();
#endif
    if (++destroyCount != 1)
        std::abort();
    auto* instance = static_cast<GainInstance*>(rawInstance);
    if (instance == nullptr || host == nullptr || host->deallocate == nullptr)
        return;
    instance->~GainInstance();
    host->deallocate(
        host->hostContext,
        instance,
        sizeof(GainInstance),
        alignof(GainInstance));
}



void unload(void*, const TalasKernelAbiHostServicesV1*)
{
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE || \
    TALAS_COMPONENT_ACTIVATION_PROBE_COMPATIBLE
    if (destroyCount == 0)
    {
        if (++unloadCount != 1)
            std::abort();
        return;
    }
#endif
    if (destroyCount != 1 || ++unloadCount != 1)
        std::abort();
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 22
    throw 22;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 23
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 24
    hangForever();
#endif
}

TalasKernelAbiCompatibilityV1 compatibility()
{
    TalasKernelAbiCompatibilityV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.pointerBits = sizeof(void*) * 8U;
    value.littleEndian =
        std::endian::native == std::endian::little ? 1U : 0U;
    value.doubleBits = sizeof(double) * 8U;
    value.doubleIec559 =
        std::numeric_limits<double>::is_iec559 ? 1U : 0U;
    value.maximumAlignment = alignof(std::max_align_t);
    return value;
}
}

#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 1
#define TALAS_COMPONENT_ACTIVATION_ENTRY_SYMBOL \
    talas_component_bundle_missing_entry_fixture_v1
#else
#define TALAS_COMPONENT_ACTIVATION_ENTRY_SYMBOL talas_component_bundle_entry_v1
#endif

extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 TALAS_COMPONENT_ACTIVATION_ENTRY_SYMBOL(
    const TalasKernelAbiHostServicesV1*,
    const TalasKernelAbiModuleV1** output)
{
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 3
    return TALAS_KERNEL_ABI_STATUS_FATAL;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 4
    throw 4;
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 5
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 6
    hangForever();
#elif TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 16
    return static_cast<TalasKernelAbiStatusV1>(UINT32_MAX);
#endif
    if (output == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    static const TalasKernelAbiDescriptorV1 descriptor = [] {
        TalasKernelAbiDescriptorV1 value{};
        value.structSize = sizeof(value);
        value.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        value.typeId = {
            "org.talas.activation.fixture.gain",
            sizeof("org.talas.activation.fixture.gain") - 1U,
        };
        value.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET
            | (TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE ? TALAS_KERNEL_ABI_CAPABILITY_STATEFUL : 0);
        value.create = create;
        value.prepare = prepare;
        value.reset = reset;
        value.process = process;
        value.destroy = destroy;
        return value;
    }();
    static const TalasKernelAbiModuleV1 module = [] {
        TalasKernelAbiModuleV1 value{};
        value.structSize = sizeof(value);
        value.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 2
        value.moduleId = {
            "org.talas.activation.fixture.mismatch",
            sizeof("org.talas.activation.fixture.mismatch") - 1U,
        };
#else
        value.moduleId = {
            "org.talas.activation.fixture",
            sizeof("org.talas.activation.fixture") - 1U,
        };
#endif
        value.moduleVersion = {
            TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION,
            sizeof(TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION) - 1U,
        };
        value.minimumApiVersion = TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE
            ? TALAS_KERNEL_API_VERSION_V4
            : (TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE
                ? TALAS_KERNEL_API_VERSION_V3
                : (TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE
                    ? TALAS_KERNEL_API_VERSION_V2
                    : TALAS_KERNEL_API_VERSION_V1));
        value.maximumApiVersion = value.minimumApiVersion;
        value.compatibility = compatibility();
        value.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
        value.trustClass = TALAS_KERNEL_ABI_TRUST_UNTRUSTED;
        value.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET
            | (TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE ? TALAS_KERNEL_ABI_CAPABILITY_STATEFUL : 0);
        value.kernels = &descriptor;
        value.kernelCount = 1;
        value.unload = unload;
        return value;
    }();
    *output = &module;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE
namespace
{
struct ResourceKernelCheckpoint
{
    GainInstance* owner;
    std::array<double, 2> history;
    std::uint32_t processCalls;
    std::uint32_t resetCalls;
};
TalasKernelAbiStatusV1 captureResourceKernel(void* raw, const TalasKernelAbiHostServicesV1* host, void** output)
{
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 3
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#elif TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 4
    std::abort();
#endif
    auto* instance = static_cast<GainInstance*>(raw);
    if (instance->checkpointHeld) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 7
    if (!instance->checkpointCaptureFailedOnce)
    {
        instance->checkpointCaptureFailedOnce = true;
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    }
#endif
    auto* memory = host->allocate(host->hostContext, sizeof(ResourceKernelCheckpoint), alignof(ResourceKernelCheckpoint));
    if (!memory) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    *output = new (memory) ResourceKernelCheckpoint{instance, instance->checkpointHistory,
        instance->processCalls, instance->resetCalls};
    instance->checkpointHeld = true;
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 8
    ++instance->checkpointCaptureOrdinal;
#endif
    return TALAS_KERNEL_ABI_STATUS_OK;
}
TalasKernelAbiStatusV1 restoreResourceKernel(void* raw, const void* payload)
{
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 5
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#elif TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 6
    std::abort();
#elif TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 2
    return TALAS_KERNEL_ABI_STATUS_FATAL;
#else
    auto* instance = static_cast<GainInstance*>(raw);
    const auto& state = *static_cast<const ResourceKernelCheckpoint*>(payload);
    if (state.owner != instance) return TALAS_KERNEL_ABI_STATUS_FATAL;
    instance->checkpointHistory = state.history;
    instance->processCalls = state.processCalls;
    instance->resetCalls = state.resetCalls;
    return TALAS_KERNEL_ABI_STATUS_OK;
#endif
}
void destroyResourceKernelCheckpoint(void* payload, const TalasKernelAbiHostServicesV1* host)
{
    auto* state = static_cast<ResourceKernelCheckpoint*>(payload);
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE == 8
    // A committed prefix survives; finalizing the next checkpoint kills the
    // helper so the public transaction must observe terminal commit failure.
    if (state->owner->checkpointCaptureOrdinal == 2) std::abort();
#endif
    state->owner->checkpointHeld = false;
    state->~ResourceKernelCheckpoint();
    host->deallocate(host->hostContext, payload, sizeof(ResourceKernelCheckpoint), alignof(ResourceKernelCheckpoint));
}
}
extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_checkpoint_entry_v1(const TalasKernelAbiCheckpointExtensionV1** output)
{
    static const TalasKernelAbiCheckpointDescriptorV1 descriptor{
        sizeof(TalasKernelAbiCheckpointDescriptorV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
        {"org.talas.activation.fixture.gain", sizeof("org.talas.activation.fixture.gain") - 1},
        captureResourceKernel, restoreResourceKernel, destroyResourceKernelCheckpoint, 0, 0};
    static const TalasKernelAbiCheckpointExtensionV1 extension{
        sizeof(TalasKernelAbiCheckpointExtensionV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
        {"org.talas.activation.fixture", sizeof("org.talas.activation.fixture") - 1},
        {TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION, sizeof(TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION) - 1},
        &descriptor, 1, 0, 0};
    *output = &extension;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
#endif
