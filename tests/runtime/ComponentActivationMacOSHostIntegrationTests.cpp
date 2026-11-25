#include "runtime/component_activation/macos/ComponentActivationMacOSHost.h"

#ifndef TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION
#define TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO
#define TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO 1
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_FAILURE_MODE
#define TALAS_COMPONENT_ACTIVATION_FAILURE_MODE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE
#define TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE 48000
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES
#define TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES 256
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS
#define TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS 1
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_AUDIO_REVOKE_BEFORE_READ
#define TALAS_COMPONENT_ACTIVATION_AUDIO_REVOKE_BEFORE_READ 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE
#define TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_AUDIO_PARTITION_FRAMES
#define TALAS_COMPONENT_ACTIVATION_AUDIO_PARTITION_FRAMES 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH
#define TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_AUDIO_CANCEL_ACTIVE_PEER
#define TALAS_COMPONENT_ACTIVATION_AUDIO_CANCEL_ACTIVE_PEER 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REVOKE_BEFORE_READ
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REVOKE_BEFORE_READ 0
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH
#define TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH 0
#endif

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 1 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 7 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 8 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 9 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 11 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 12 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"
#endif

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16 || \
    (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 && \
        TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH) || \
    (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18 && \
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH)
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <new>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <libproc.h>
#include <sys/fcntl.h>
#include <sys/proc_info.h>
#include <unistd.h>

// Compiled once per integration group or fixture mode; helpers that the
// current configuration does not use are expected.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 14 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 22
namespace
{
thread_local bool countWholeAllocations = false;
thread_local std::size_t wholeAllocationCount = 0;
thread_local bool failWholeAllocation = false;
thread_local bool wholeAllocationFaultTriggered = false;
thread_local std::size_t completedRealBlocks = 0;
thread_local std::size_t faultAfterRealBlocks = 0;
thread_local bool auditWholeFinalization = false;
void recordWholeAllocation()
{
    if (countWholeAllocations) ++wholeAllocationCount;
    if (failWholeAllocation && completedRealBlocks >= faultAfterRealBlocks)
    {
        failWholeAllocation = false;
        wholeAllocationFaultTriggered = true;
        throw std::bad_alloc{};
    }
}
}
void* operator new(std::size_t size)
{
    recordWholeAllocation();
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

namespace
{
namespace activation = talas::runtime::component_activation;
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template <typename Ready>
bool waitForPublishedAudioResponse(
    Ready&& ready,
    std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return ready();
}

bool waitForTerminalState(
    const activation::ComponentActivationMacOSHost& host,
    activation::ComponentActivationMacOSHostStateV1 expected,
    std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto state = host.state();
        if (state == expected)
            return true;
        if (state
            == activation::ComponentActivationMacOSHostStateV1::Quarantined)
        {
            if (expected
                != activation::ComponentActivationMacOSHostStateV1::
                    Quarantined)
            {
                std::cerr << "unexpected activation quarantine: diagnostic="
                          << static_cast<unsigned>(host.diagnostic()) << '\n';
            }
            return expected
                == activation::ComponentActivationMacOSHostStateV1::
                    Quarantined;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

activation::ComponentActivationMacOSEndpointV1 endpoint()
{
    switch (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO)
    {
        case 1:
            return activation::ComponentActivationMacOSEndpointV1::Production;
        case 2:
            return activation::ComponentActivationMacOSEndpointV1::
                FixtureMalformed;
        case 3:
            return activation::ComponentActivationMacOSEndpointV1::FixtureHang;
        case 4:
            return activation::ComponentActivationMacOSEndpointV1::
                FixtureSessionMismatch;
        case 5:
            return activation::ComponentActivationMacOSEndpointV1::FixtureMissing;
        case 6:
            return activation::ComponentActivationMacOSEndpointV1::
                FixtureHeartbeatHang;
        case 7:
        case 8:
        case 9:
        case 10:
            return activation::ComponentActivationMacOSEndpointV1::Production;
        case 11:
            return activation::ComponentActivationMacOSEndpointV1::
                FixtureTerminationFailure;
        case 12:
            return activation::ComponentActivationMacOSEndpointV1::
                FixtureProcessIdentityMismatch;
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
            return activation::ComponentActivationMacOSEndpointV1::Production;
    }
    return activation::ComponentActivationMacOSEndpointV1::FixtureMissing;
}

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 1 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 7 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 8 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 9 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 11 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 12 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
std::string digestHex(
    const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2U] = hexadecimal[(digest[index] >> 4U) & 0xfU];
        result[index * 2U + 1U] = hexadecimal[digest[index] & 0xfU];
    }
    return result;
}

bundle::ComponentBundleSnapshot activationSnapshot()
{
    std::ifstream stream(
        TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH,
        std::ios::binary);
    require(stream.good(), "activation gain fixture should open");
    const std::vector<std::uint8_t> entry{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    require(!entry.empty(), "activation gain fixture should have bytes");
    const auto entryDigest =
        activation::calculateComponentActivationByteDigestV1(entry);

    std::ostringstream json;
    json << R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.talas.activation.fixture",
  "semanticVersion":"1.0.0",
)json";
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    json << R"json(  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":3,"apiMaximum":3},
  "roles":["bounded-audio-stream-consumer","realtime-processor"],
  "permissions":["bounded-audio-stream-read","realtime-audio"],
)json";
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 1
    json << R"json(  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":4,"apiMaximum":4},
  "roles":["immutable-resource-consumer","realtime-processor"],
  "permissions":["immutable-resource-read","realtime-audio"],
)json";
#else
    json << R"json(  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":4,"apiMaximum":4},
  "roles":["bounded-audio-stream-consumer","immutable-resource-consumer","realtime-processor"],
  "permissions":["bounded-audio-stream-read","immutable-resource-read","realtime-audio"],
)json";
#endif
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
    json << R"json(  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":2,"apiMaximum":2},
  "roles":["immutable-resource-consumer","realtime-processor"],
  "permissions":["immutable-resource-read","realtime-audio"],
)json";
#else
    json << R"json(
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":1,"apiMaximum":1},
  "roles":["realtime-processor"],
  "permissions":["realtime-audio"],
)json";
#endif
    json << R"json(
  "entry":{"path":"Payload/gain.bundle","bytes":)json"
         << entry.size() << R"json(,"sha256":")json"
         << digestHex(entryDigest) << R"json("},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.fixture.gain",
    "stateSchemaVersion":1,
    "inputs":[)json";
    for (std::uint16_t channel = 0;
         channel < TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
         ++channel)
    {
        if (channel != 0)
            json << ',';
        json << R"json({"id":"in)json" << channel
             << R"json(","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true})json";
    }
    json << R"json(],
    "outputs":[)json";
    for (std::uint16_t channel = 0;
         channel < TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
         ++channel)
    {
        if (channel != 0)
            json << ',';
        json << R"json({"id":"out)json" << channel
             << R"json(","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true})json";
    }
    json << R"json(],
    "parameters":[)json";
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    json << R"json({"id":"audio","kind":"resource","domain":"scalar","choices":[]})json";
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 1
    json << R"json({"id":"table","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary","kind":"resource","domain":"scalar","choices":[]})json";
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 2
    json << R"json({"id":"table","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-a","kind":"resource","domain":"scalar","choices":[]})json";
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 3
    json << R"json({"id":"binary","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-b","kind":"resource","domain":"scalar","choices":[]})json";
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 4
    json << R"json({"id":"table-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"table-b","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary-b","kind":"resource","domain":"scalar","choices":[]},
      {"id":"table-c","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary-c","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-b","kind":"resource","domain":"scalar","choices":[]})json";
#else
    json << R"json({"id":"table","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-b","kind":"resource","domain":"scalar","choices":[]})json";
#endif
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
    json << R"json({"id":"table","kind":"resource","domain":"scalar","choices":[]})json";
#else
    json << R"json({"id":"gain","kind":"expression","domain":"scalar","choices":[]})json";
#endif
    json << R"json(]
  }],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Talas tests","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
    const auto parsed = bundle::parseComponentBundleManifest(json.str());
    require(parsed.ok(), "activation gain manifest should parse");
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        *parsed.manifest);
    require(canonical.ok(), "activation gain manifest should canonicalize");
    bundle::ComponentBundleSnapshot snapshot;
    snapshot.manifest = *parsed.manifest;
    snapshot.canonicalManifest = canonical.canonicalJson;
    snapshot.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        snapshot.manifest.entry.path,
        entryDigest,
        entry,
        true,
    });
    snapshot.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    require(
        bundle::validateComponentBundleSnapshotIdentity(snapshot),
        "activation gain snapshot should be exact");
    return snapshot;
}

std::size_t activationMaterializationCount()
{
    std::array<char, PATH_MAX> temporaryRoot{};
    const auto rootBytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        temporaryRoot.data(),
        temporaryRoot.size());
    require(
        rootBytes != 0 && rootBytes <= temporaryRoot.size(),
        "activation temp root should resolve");
    constexpr std::string_view prefix = "TalasComponentActivation.v1.";
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             temporaryRoot.data()))
    {
        const auto name = entry.path().filename().string();
        if (name.starts_with(prefix))
            ++count;
    }
    return count;
}

bool waitForMaterializationCount(
    std::size_t expected,
    std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (activationMaterializationCount() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return activationMaterializationCount() == expected;
}

bool waitForPeerExit(
    std::int32_t peer,
    std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        errno = 0;
        if (peer <= 0 || (kill(peer, 0) != 0 && errno == ESRCH))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    errno = 0;
    return peer <= 0 || (kill(peer, 0) != 0 && errno == ESRCH);
}

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 1
bool peerHoldsWritableMaterialization(std::int32_t peer)
{
    std::vector<proc_fdinfo> descriptors(512);
    const auto bytes = proc_pidinfo(
        peer,
        PROC_PIDLISTFDS,
        0,
        descriptors.data(),
        static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
    require(bytes > 0, "the helper's descriptors should be inspectable");
    const auto count = static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (descriptors[index].proc_fdtype != PROX_FDTYPE_VNODE)
            continue;
        vnode_fdinfowithpath information{};
        if (proc_pidfdinfo(
                peer,
                descriptors[index].proc_fd,
                PROC_PIDFDVNODEPATHINFO,
                &information,
                sizeof(information))
                == static_cast<int>(sizeof(information))
            && std::string_view{information.pvip.vip_path}.find(
                   "TalasComponentActivation.v1.")
                != std::string_view::npos
            && (information.pfi.fi_openflags & FWRITE) != 0)
        {
            return true;
        }
    }
    return false;
}
#endif

bool peerHasExited(std::int32_t peer)
{
    if (peer <= 0)
        return true;
    errno = 0;
    if (kill(peer, 0) != 0)
        return errno == ESRCH;
    // kill() still finds an exited helper until launchd, its parent, reaps
    // the zombie, which can take a while on a loaded machine; proc_pidinfo,
    // like the host's own identity check, no longer does.
    proc_bsdinfo info{};
    return proc_pidinfo(peer, PROC_PIDTBSDINFO, 0, &info, sizeof(info))
        != static_cast<int>(sizeof(info));
}

std::array<activation::ComponentActivationStaticParameterV1, 1>
gainParameters(double gain = 0.5)
{
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Scalar);
    parameter.idBytes = 4;
    std::copy_n("gain", 4, parameter.id.begin());
    parameter.scalarValue = gain;
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

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
std::array<activation::ComponentActivationStaticParameterV1, 1>
audioParameters()
{
    activation::ComponentActivationStaticParameterV1 parameter;
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Voice-A";
    parameter.idBytes = parameterId.size();
    parameter.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), parameter.text.begin());
    return {parameter};
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

std::vector<std::uint8_t> audioSourceBytes()
{
    constexpr std::uint16_t channels = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
    constexpr std::uint32_t sampleRate = 48'000;
    constexpr std::uint32_t frameCount = 8'192;
    constexpr auto dataBytes = frameCount * channels * 2U;
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
    for (std::uint32_t frame = 0; frame < frameCount; ++frame)
    {
        for (std::uint16_t channel = 0; channel < channels; ++channel)
        {
            appendU16(
                bytes,
                static_cast<std::uint16_t>(frame + channel * 4'096U));
        }
    }
    return bytes;
}
#endif

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
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

#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 1
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "binary", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
};
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 2
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "table", "Tables/Curve-A", ResourceSetFixtureKind::Table},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
};
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 3
constexpr std::array resourceSetFixtureMembers{
    ResourceSetFixtureMember{
        "binary", "Binary/Blob-A", ResourceSetFixtureKind::Binary},
    ResourceSetFixtureMember{
        "audio-a", "Audio/Voice-A", ResourceSetFixtureKind::Audio},
    ResourceSetFixtureMember{
        "audio-b", "Audio/Voice-B", ResourceSetFixtureKind::Audio},
};
#elif TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 4
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

std::array<
    activation::ComponentActivationStaticParameterV1,
    resourceSetFixtureMembers.size()>
resourceSetParameters()
{
    std::array<
        activation::ComponentActivationStaticParameterV1,
        resourceSetFixtureMembers.size()>
        result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        auto& parameter = result[index];
        const auto& member = resourceSetFixtureMembers[index];
        parameter.kind = static_cast<std::uint32_t>(
            activation::ComponentActivationStaticParameterKindV1::Resource);
        parameter.idBytes = member.parameterId.size();
        parameter.textBytes = member.resourceId.size();
        std::copy(
            member.parameterId.begin(),
            member.parameterId.end(),
            parameter.id.begin());
        std::copy(
            member.resourceId.begin(),
            member.resourceId.end(),
            parameter.text.begin());
    }
    return result;
}
#endif

probe::ComponentBundleProbeRequestV1 probeRequest(
    const bundle::ComponentBundleSnapshot& snapshot,
    const ComponentSignatureEvidenceView& signature)
{
    probe::ComponentBundleProbeRequestV1 request;
    request.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.bundleIdBytes = snapshot.manifest.bundleId.size();
    request.semanticVersionBytes = snapshot.manifest.semanticVersion.size();
    std::copy(
        snapshot.manifest.bundleId.begin(),
        snapshot.manifest.bundleId.end(),
        request.bundleId.begin());
    std::copy(
        snapshot.manifest.semanticVersion.begin(),
        snapshot.manifest.semanticVersion.end(),
        request.semanticVersion.begin());
    request.contentDigest = snapshot.contentDigest;
    request.signerIdentityDigest = signature.signerIdentityDigest;
    request.requestedPermissions = snapshot.manifest.requestedPermissions;
    request.declaredRoles = snapshot.manifest.declaredRoles;
    request.policyVersion = currentComponentTrustPolicyVersion;
    request.entryByteCount = snapshot.manifest.entry.byteCount;
    request.artifactGeneration = 79;
    request.validationNonce.fill(0x5c);
    return request;
}

struct ActivationAuthorityFixture
{
    bundle::ComponentBundleSnapshot snapshot = activationSnapshot();
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    decltype(resourceSetParameters()) parameters = resourceSetParameters();
#else
    std::array<activation::ComponentActivationStaticParameterV1, 1>
        parameters = gainParameters();
#endif
    ComponentSignatureEvidence signature;
    bundle::ComponentOwnerAuthorizationStore owner;
    ComponentApprovalFingerprint candidate;
    probe::ComponentBundleProbeEvidenceAuthoringV1 probeAuthoring;
    activation::ComponentActivationAuthorizationInputsV1 inputs;
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    std::optional<activation::ComponentActivationFrozenAudioSourceV1>
        audioSource;
#endif
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    std::array<activation::ComponentActivationFrozenAudioSourceV1, 2>
        resourceSetAudioSources;
    std::array<
        talas::core::ResourceSnapshotPtr,
        resourceSetFixtureMembers.size()>
        resourceSetSnapshots{};
    std::array<
        activation::ComponentActivationResourceSetInputV1,
        resourceSetFixtureMembers.size()>
        resourceSetInputs{};
#endif

    explicit ActivationAuthorityFixture(
        activation::ComponentActivationMacOSHost& host)
    {
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
        parameters = audioParameters();
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
        parameters = resourceParameters();
#endif
        ComponentSignatureEvidenceView signatureView;
        signatureView.state = ComponentSignatureState::Verified;
        signatureView.contentDigest = snapshot.contentDigest;
        signatureView.signerIdentityDigest.fill(0x42);
        signatureView.profileVersion = currentComponentSignatureProfileVersion;
        signatureView.hasContentDigest = true;
        signatureView.hasSignerIdentity = true;
        signature = bundle::ComponentBundleSignatureEvidenceFactory::make(
            signatureView);

        candidate.bundleId = snapshot.manifest.bundleId;
        candidate.semanticVersion = snapshot.manifest.semanticVersion;
        candidate.contentDigest = snapshot.contentDigest;
        candidate.signerIdentityDigest = signatureView.signerIdentityDigest;
        candidate.requestedPermissions = snapshot.manifest.requestedPermissions;
        candidate.policyVersion = currentComponentTrustPolicyVersion;
        candidate.hasContentDigest = true;
        candidate.hasSignerIdentity = true;
        require(
            owner.recordOwnerAction(
                     candidate,
                     bundle::ComponentOwnerAuthorizationAction::Approve,
                     owner.generation())
                .ok(),
            "activation owner approval should apply");

        const auto request = probeRequest(snapshot, signatureView);
        probe::ComponentBundleProbeResultV1 result;
        result.header = probe::componentBundleProbeHeaderV1(
            probe::ComponentBundleProbeRecordKindV1::Result,
            sizeof(result));
        result.state = static_cast<std::uint32_t>(ComponentProbeState::Clean);
        result.diagnostic = static_cast<std::uint32_t>(
            probe::ComponentBundleProbeResultDiagnosticV1::Ready);
        result.contentDigest = request.contentDigest;
        result.artifactGeneration = request.artifactGeneration;
        result.validationNonce = request.validationNonce;
        auto authored = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(result, request);
        require(authored.has_value(), "activation probe should author");
        probeAuthoring = std::move(*authored);

        inputs.snapshot = &snapshot;
        inputs.signature = signature;
        inputs.ownerAuthorization = owner.authorizationEvidence();
        inputs.probe = probeAuthoring.evidence;
        inputs.runtime = host.runtimeEvidence();
        inputs.validationNonce = request.validationNonce;
        inputs.descriptorId = "org.talas.activation.fixture.gain";
        inputs.artifactGeneration = request.artifactGeneration;
        inputs.sampleRate = TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE;
        inputs.blockFrames = TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
        inputs.inputChannelCount = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
        inputs.outputChannelCount = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
        inputs.staticParameters = parameters;
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
        const auto sourceBytes = audioSourceBytes();
        const auto sourcePath = std::filesystem::path{"/private/tmp"}
            / ("talas-component-activation-audio-"
                + std::to_string(getpid()) + ".wav");
        {
            std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
            source.write(
                reinterpret_cast<const char*>(sourceBytes.data()),
                static_cast<std::streamsize>(sourceBytes.size()));
            require(source.good(), "Audio source fixture should write exactly");
        }
        talas::core::ResourceSnapshot resource;
        resource.id = "Audio/Voice-A";
        resource.kind = talas::core::ResourceSnapshotKind::Audio;
        resource.generation = 7;
        resource.contentHash =
            talas::project::calculateProjectAssetSha256(sourceBytes);
        resource.resolvedPath = sourcePath;
        resource.channelCount = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
        resource.sampleRate = 48'000;
        resource.frameCount = 8'192;
        resource.encoding = talas::project::pcm16WavAssetEncoding;
        auto frozen =
            activation::ComponentActivationAudioSourceFreezerV1::freeze(
                resource);
        require(frozen.ok(), "Audio source fixture should freeze exactly");
        audioSource.emplace(std::move(*frozen.source));
        std::error_code sourceCleanupError;
        std::filesystem::remove(sourcePath, sourceCleanupError);
        require(!sourceCleanupError,
            "Audio source path should disappear before XPC transfer");
        inputs.resourceSnapshot = std::make_shared<
            const talas::core::ResourceSnapshot>(std::move(resource));
        inputs.audioSource = &*audioSource;
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
        std::size_t audioIndex = 0;
        for (std::size_t index = 0;
             index < resourceSetFixtureMembers.size();
             ++index)
        {
            const auto& definition = resourceSetFixtureMembers[index];
            talas::core::ResourceSnapshot resource;
            resource.id = definition.resourceId;
            resource.generation = 7U + index;
            if (definition.kind == ResourceSetFixtureKind::Table)
            {
                resource.kind = talas::core::ResourceSnapshotKind::Table;
                resource.contentHash = std::string(64, '6');
                resource.tableSamples = {0.25, 0.5, 0.75};
            }
            else if (definition.kind == ResourceSetFixtureKind::Binary)
            {
                resource.kind = talas::core::ResourceSnapshotKind::Binary;
                resource.binaryBytes = {0x00, 0x7f, 0x80, 0xfe, 0xff};
                resource.contentHash = digestHex(
                    activation::calculateComponentActivationByteDigestV1(
                        resource.binaryBytes));
            }
            else
            {
                require(
                    audioIndex < resourceSetAudioSources.size(),
                    "resource-set fixture must remain within two Audio members");
                const auto sourceBytes = audioSourceBytes();
                const auto sourcePath = std::filesystem::path{"/private/tmp"}
                    / ("talas-component-activation-audio-"
                        + std::to_string(getpid()) + "-"
                        + std::to_string(audioIndex) + ".wav");
                {
                    std::ofstream source(
                        sourcePath, std::ios::binary | std::ios::trunc);
                    source.write(
                        reinterpret_cast<const char*>(sourceBytes.data()),
                        static_cast<std::streamsize>(sourceBytes.size()));
                    require(
                        source.good(),
                        "resource-set Audio source fixture should write exactly");
                }
                resource.kind = talas::core::ResourceSnapshotKind::Audio;
                resource.contentHash =
                    talas::project::calculateProjectAssetSha256(sourceBytes);
                resource.resolvedPath = sourcePath;
                resource.channelCount = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
                resource.sampleRate = 48'000;
                resource.frameCount = 8'192;
                resource.encoding = talas::project::pcm16WavAssetEncoding;
                auto frozen = activation::
                    ComponentActivationAudioSourceFreezerV1::freeze(resource);
                require(
                    frozen.ok(),
                    "resource-set Audio source fixture should freeze exactly");
                resourceSetAudioSources[audioIndex] =
                    std::move(*frozen.source);
                std::error_code sourceCleanupError;
                std::filesystem::remove(sourcePath, sourceCleanupError);
                require(
                    !sourceCleanupError,
                    "resource-set Audio source path should disappear before transfer");
            }
            resourceSetSnapshots[index] =
                std::make_shared<const talas::core::ResourceSnapshot>(
                    std::move(resource));
            resourceSetInputs[index] = {
                resourceSetSnapshots[index],
                definition.kind == ResourceSetFixtureKind::Audio
                    ? &resourceSetAudioSources[audioIndex++]
                    : nullptr,
            };
        }
        inputs.resourceSetInputs = resourceSetInputs;
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
        talas::core::ResourceSnapshot resource;
        resource.id = "Tables/Curve-A";
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
        resource.kind = talas::core::ResourceSnapshotKind::Binary;
        resource.generation = 7;
        resource.resolvedPath = "/private/project/Tables/Curve-A.binary";
        resource.binaryBytes = {0x00, 0x7f, 0x80, 0xfe, 0xff};
        resource.contentHash = digestHex(
            activation::calculateComponentActivationByteDigestV1(
                resource.binaryBytes));
#else
        resource.kind = talas::core::ResourceSnapshotKind::Table;
        resource.generation = 7;
        resource.contentHash = std::string(64, '6');
        resource.resolvedPath = "/private/project/Tables/Curve-A.table";
        resource.tableSamples = {0.25, 0.5, 0.75};
#endif
        inputs.resourceSnapshot = std::make_shared<
            const talas::core::ResourceSnapshot>(std::move(resource));
#endif
    }

    std::optional<activation::ComponentActivationLeaseV1> lease(
        std::uint64_t generation)
    {
        auto current = inputs;
        current.activationGeneration = generation;
        current.authorizationNonce.fill(
            static_cast<std::uint8_t>(generation));
        auto authorized =
            activation::ComponentActivationAuthorizationFactory::make(current);
        if (!authorized.ok())
            return std::nullopt;
        return authorized.authority->consume();
    }
};

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16 || \
    (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 && \
        TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH) || \
    (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18 && \
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH)
talas::project::ProjectDocument transientComponentDocument(
    const bundle::ComponentBundleSnapshot& snapshot)
{
    require(
        snapshot.manifest.nodes.size() == 1,
        "transient fixture should expose exactly one node");
    const auto& manifestNode = snapshot.manifest.nodes.front();
    talas::project::ProjectPreservedComponent capsule;
    capsule.typeVersion = snapshot.manifest.semanticVersion;
    capsule.stateSchemaVersion = manifestNode.stateSchemaVersion;
    capsule.displayName = "Prepared isolated gain";
    capsule.bundleId = snapshot.manifest.bundleId;
    for (const auto& input : manifestNode.inputs)
    {
        capsule.inputs.push_back({
            input.id,
            input.type,
            input.required,
            input.signal,
        });
    }
    for (const auto& output : manifestNode.outputs)
    {
        capsule.outputs.push_back({
            output.id,
            output.type,
            output.required,
            output.signal,
        });
    }
    for (const auto& parameter : manifestNode.parameters)
    {
        capsule.parameters.push_back({
            parameter.id,
            parameter.kind,
            parameter.domain,
            parameter.choices,
        });
    }

    std::vector<talas::project::ProjectParameterExpression> parameters;
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    parameters.reserve(resourceSetFixtureMembers.size());
    for (const auto& member : resourceSetFixtureMembers)
    {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = member.parameterId;
        parameter.resource = member.resourceId;
        parameters.push_back(std::move(parameter));
    }
#else
    talas::project::ProjectParameterExpression parameter;
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    parameter.parameterId = "audio";
    parameter.resource = "Audio/Voice-A";
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
    parameter.parameterId = "table";
    parameter.resource = "Tables/Curve-A";
#else
    parameter.parameterId = "gain";
    parameter.expression = "0.5";
#endif
    parameters.push_back(std::move(parameter));
#endif
    talas::project::ProjectNode component;
    component.instanceId = "component";
    component.typeId = manifestNode.typeId;
    component.parameters = std::move(parameters);
    component.preservedComponent = std::move(capsule);

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
        std::move(component),
    };
    for (const auto& input : manifestNode.inputs)
    {
        document.graph.connections.push_back(
            {"input", "audioOut", "component", input.id});
    }
    document.graph.connections.push_back(
        {"component", manifestNode.outputs.front().id, "out", "audioIn"});
    return document;
}

talas::project::ProjectDocument directAudioDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings transientRenderSettings(
    talas::core::ResourceSnapshotPtr resourceSnapshot = {})
{
    talas::runtime::RenderSettings result;
    result.sampleRate = TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE;
    result.maxBlockFrames = TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    if (resourceSnapshot != nullptr)
        result.resourceSnapshots.push_back(std::move(resourceSnapshot));
    return result;
}

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
talas::runtime::RenderSettings transientResourceSetRenderSettings(
    std::span<const talas::core::ResourceSnapshotPtr> resourceSnapshots)
{
    auto result = transientRenderSettings();
    result.resourceSnapshots.assign(
        resourceSnapshots.begin(), resourceSnapshots.end());
    return result;
}
#endif
#endif
#endif

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 14 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 22
// Observation only: all authority, checkpoint ownership, health and processing
// are delegated to the actual production binding and its real XPC helper.
class ObservedRealBinding final : public PreparedIsolatedComponentBinding
{
public:
    explicit ObservedRealBinding(std::shared_ptr<PreparedIsolatedComponentBinding> real) : real_(std::move(real)) {}
    const IsolatedComponentRuntimeContract& contract() const noexcept override { return real_->contract(); }
    bool active() const noexcept override { return real_->active(); }
    bool supportsOfflineCheckpoint() const noexcept override { return real_->supportsOfflineCheckpoint(); }
    class ObservedCheckpoint final : public IsolatedComponentOfflineCheckpoint
    {
    public:
        std::unique_ptr<IsolatedComponentOfflineCheckpoint> real;
        bool commit() noexcept override
        {
            const auto committed = real->commit();
            if (committed && auditWholeFinalization)
            {
                wholeAllocationCount = 0;
                countWholeAllocations = true;
                faultAfterRealBlocks = 0;
                wholeAllocationFaultTriggered = false;
                failWholeAllocation = true;
            }
            return committed;
        }
    };
    std::unique_ptr<IsolatedComponentOfflineCheckpoint> beginOfflineCheckpoint() override
    {
        auto observed = std::make_unique<ObservedCheckpoint>();
        observed->real = real_->beginOfflineCheckpoint();
        if (!observed->real) return {};
        return observed;
    }
    std::optional<IsolatedComponentAudioStreamHealthSetSnapshot> audioStreamHealthSet() noexcept override { return real_->audioStreamHealthSet(); }
    bool tryBeginStage() noexcept override { return real_->tryBeginStage(); }
    void endStage() noexcept override { real_->endStage(); }
    void quarantine(IsolatedComponentQuarantineReason reason) noexcept override { real_->quarantine(reason); }
    IsolatedComponentProcessResult process(std::span<const float* const> input, std::span<float* const> output,
        std::uint32_t frames, IsolatedComponentTransport transport) noexcept override
    {
        const auto result = real_->process(input, output, frames, transport);
        if (result == IsolatedComponentProcessResult::FirstBlockSilence || result == IsolatedComponentProcessResult::Rendered)
            ++completedRealBlocks;
        return result;
    }
private:
    std::shared_ptr<PreparedIsolatedComponentBinding> real_;
};

void testRealBindingOfflineTransactions(activation::ComponentActivationMacOSHost& host,
    ActivationAuthorityFixture& fixture)
{
    using namespace talas::runtime;
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 14 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 15
    const auto waitControl = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (host.checkpointStatus() == activation::ComponentActivationCheckpointStatusV1::Pending
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    host.holdNextCheckpointReplyForTesting();
    require(host.beginOfflineCheckpoint() == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "ownership transfer fixture must capture a host-owned checkpoint");
    const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!host.checkpointReplyHeldForTesting() && std::chrono::steady_clock::now() < heldDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(host.checkpointReplyHeldForTesting(), "ownership test holds the real capture acknowledgment");
    require(!std::move(host).takePreparedIsolatedComponentBinding("component", fixture.snapshot, fixture.parameters)
        && host.checkpointStatus() == activation::ComponentActivationCheckpointStatusV1::Pending,
        "prepared binding transfer cannot consume a pending host checkpoint command");
    host.releaseCheckpointReplyForTesting();
    waitControl();
    require(host.checkpointStatus() == activation::ComponentActivationCheckpointStatusV1::Captured,
        "host-owned checkpoint must reach its authenticated held boundary");
    const auto premature = std::move(host).takePreparedIsolatedComponentBinding(
        "component", fixture.snapshot, fixture.parameters);
    require(!premature && host.checkpointStatus() == activation::ComponentActivationCheckpointStatusV1::Captured,
        "prepared binding transfer must reject without stranding a host-owned checkpoint");
    require(host.restoreOfflineCheckpoint() == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "rejected transfer preserves the original host checkpoint owner");
    waitControl();
    require(host.checkpointStatus() == activation::ComponentActivationCheckpointStatusV1::Restored,
        "the original owner can restore before retrying prepared binding transfer");
#endif
    auto binding = std::move(host).takePreparedIsolatedComponentBinding(
        "component", fixture.snapshot, fixture.parameters);
    require(binding && binding->active(), "real prepared binding transfer must retain live authority");
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 17 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 22
    require(!binding->supportsOfflineCheckpoint() && !binding->beginOfflineCheckpoint(),
        "authenticated legacy discovery must retain absent whole-call capability");
    std::array<float, 256> input{}, output{};
    input.fill(1.0F);
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 22
    require(binding->process(inputs, outputs, 256, {7, 0, true}) == IsolatedComponentProcessResult::FirstBlockSilence,
        "ordinary realtime submission must return first-block silence while its real helper is hung");
    require(binding->process(inputs, outputs, 256, {7, 256, false}) == IsolatedComponentProcessResult::Quarantined
        && !binding->active() && std::ranges::all_of(output, [](float value) { return value == 0.0F; }),
        "ordinary realtime next-block deadline remains fail-closed without offline completion waiting");
#else
    for (std::size_t block = 0; block < 2; ++block)
    {
        require(binding->process(inputs, outputs, 256, {7, static_cast<std::int64_t>(block * 256), block == 0})
            == (block == 0 ? IsolatedComponentProcessResult::FirstBlockSilence : IsolatedComponentProcessResult::Rendered),
            "legacy binding retains its existing noncheckpoint processing profile");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            require(std::abs(output[frame] - (block == 0 ? 0.0 : static_cast<double>(frame) / 32767.0)) < 1e-6,
                "legacy discovery preserves exact one-block resource audio");
        require(waitForPublishedAudioResponse([&] { return binding->publishedAudioResponseReadyForTesting(); }),
            "legacy fixture explicitly observes its next normal realtime deadline boundary");
    }
#endif
    binding.reset();
    return;
#endif
    require(binding->supportsOfflineCheckpoint(),
        "the real prepared binding must advertise its authenticated complete checkpoint participant");
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 14 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 15
    failWholeAllocation = true;
    wholeAllocationFaultTriggered = false;
    faultAfterRealBlocks = completedRealBlocks;
    bool captureThrew = false;
    try { auto unused = binding->beginOfflineCheckpoint(); }
    catch (const std::bad_alloc&) { captureThrew = true; }
    failWholeAllocation = false;
    require(captureThrew && wholeAllocationFaultTriggered && binding->active(),
        "caller snapshot allocation failure precedes helper capture and leaves authority usable");
    require(binding->tryBeginStage(), "stage exclusion fixture must acquire publication authority");
    require(!binding->beginOfflineCheckpoint(), "stage-held capture rejects before scheduling a control wait");
    binding->endStage();
    for (bool commit : {false, true})
    {
        auto checkpoint = binding->beginOfflineCheckpoint();
        require(checkpoint && !binding->beginOfflineCheckpoint(), "real nested capture preserves the original checkpoint");
        wholeAllocationCount = 0;
        countWholeAllocations = true;
        const auto committed = !commit || checkpoint->commit();
        checkpoint.reset();
        countWholeAllocations = false;
        require(committed && wholeAllocationCount == 0 && binding->active(),
            "real caller commit/restore and cleanup allocate no C++ objects");
    }
#endif
    auto document = transientComponentDocument(fixture.snapshot);
    document.graph.nodes[1] = {"input", "talas.noise", {}};
    document.graph.nodes.push_back({"gain", "talas.vca", {}});
    document.graph.nodes.push_back({"control", "talas.constant", {{"value", "@Gain / @Divisor"}}});
    document.graph.connections.back() = {"component", fixture.snapshot.manifest.nodes.front().outputs.front().id, "gain", "audioIn"};
    document.graph.connections.push_back({"control", "value", "gain", "gain"});
    document.graph.connections.push_back({"gain", "audioOut", "out", "audioIn"});
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"Gain", "Gain", talas::expr::ValueDomain::Scalar, 1.0},
        {"Divisor", "Divisor", talas::expr::ValueDomain::Scalar, 1.0}};
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    auto renderSettings = transientResourceSetRenderSettings(fixture.resourceSetSnapshots);
#else
    auto renderSettings = transientRenderSettings(fixture.inputs.resourceSnapshot);
#endif
    renderSettings.enableRealtimeAnalysisCapture = true;
    renderSettings.realtimeAnalysisQueueCapacity = 8;
    renderSettings.realtimeAnalysisTargets.push_back({{}, {}, "gain", "audioOut", RealtimeAnalysisKind::Oscilloscope,
        RealtimeAnalysisPayloadKind::AudioBlock, 1, "real-offline-rollback"});
    auto observedBinding = std::make_shared<ObservedRealBinding>(binding);
    auto compiled = compileProjectWithIsolatedComponent(document, talas::core::makeBuiltinNodeRegistry(), renderSettings, observedBinding);
    require(compiled.ok(), "real isolated offline graph must compile with numeric combined input");
    {
        OfflineRenderSession session(*compiled.graph);
        std::array<OfflineMidiVoiceEvent, 2> events{};
        for (std::size_t index = 0; index < events.size(); ++index)
        {
            events[index].kind = OfflineMidiVoiceEventKind::VariableAssignment;
            events[index].sequence = events[index].batchId = index + 1;
        }
        events[0].frame = 512; events[0].target.variableId = "Gain"; events[0].value = 0.5;
        events[1].frame = 8448; events[1].target.variableId = "Divisor"; events[1].value = 0.0;
        require(session.scheduleMidiVoiceEvents(events), "real offline input must retain exact-block scheduling");
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 16 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 20
        const auto beforeCapture = completedRealBlocks;
        const auto captureFailure = session.render(256);
        require(!captureFailure.ok() && captureFailure.mono.empty() && captureFailure.channels.empty()
            && completedRealBlocks == beforeCapture, "failed helper capture must reject before any real processing");
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 20
        require(!binding->active() && !binding->audioStreamHealthSet(), "capture timeout retires live binding and cached health");
        require(!session.render(256).ok(), "capture timeout cannot become a reusable offline retry");
        return;
#else
        require(binding->active(), "ordinary helper capture failure leaves an exact retry available");
#endif
#endif
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 14 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 15
        auditWholeFinalization = true;
#endif
        const auto prefix = session.render(256);
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 14 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 15
        auditWholeFinalization = false;
        failWholeAllocation = false;
        countWholeAllocations = false;
        require(!wholeAllocationFaultTriggered && wholeAllocationCount == 0 && prefix.ok() && binding->active(),
            "entire real public finalization after helper commit must allocate no caller C++ objects");
#endif
        require(prefix.ok() && prefix.mono.size() == 256
            && std::ranges::all_of(prefix.mono, [](double value) { return value == 0.0; }),
            "real offline prefix commits the exact first-block silent pipeline boundary");
        const auto beforeHealth = binding->audioStreamHealthSet();
        require(beforeHealth.has_value(), "real offline prefix exposes authenticated resource health");
        const auto capture = compiled.graph->analysisCapture();
        const auto prefixAnalysis = capture->copyLatestRecordResult(0, {});
        require(prefixAnalysis.status == RealtimeAnalysisCopyStatus::DestinationTooSmall,
            "whole-call rollback fixture retains one unread committed analysis record");
        const auto prefixDrops = capture->droppedBlockCount(0);
        const auto prefixRejections = capture->rejectedRecordCount(0);
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 18 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 19 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 21
        const auto failed = session.render(TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 19 ? 8192 : 8448);
        require(!failed.ok() && failed.mono.empty() && failed.channels.empty() && !binding->active(),
            "terminal real restore or commit failure rejects the entire public result and audio authority");
        const auto afterAnalysis = capture->copyLatestRecordResult(0, {});
        require(afterAnalysis.status == prefixAnalysis.status && afterAnalysis.header == prefixAnalysis.header
            && capture->droppedBlockCount(0) == prefixDrops && capture->rejectedRecordCount(0) == prefixRejections,
            "terminal real finalization cannot publish provisional analysis or counters");
        const auto completed = completedRealBlocks;
        require(!binding->audioStreamHealthSet() && !session.render(256).ok() && completedRealBlocks == completed,
            "terminal finalization hides cached health and never resumes a partly committed helper");
        return;
#endif
        std::atomic<bool> inspectionFailed{false};
        std::atomic<std::uint64_t> inspectionCount{0};
        std::jthread inspector([&](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                const auto health = binding->audioStreamHealthSet();
                if (!health || health->streamCount != beforeHealth->streamCount)
                    inspectionFailed.store(true);
                else if (health->streamCount == 2
                    && (health->streams[0].seekGeneration != health->streams[1].seekGeneration
                        || health->streams[0].health.discontinuities != health->streams[1].health.discontinuities
                        || health->streams[0].health.endOfStreamFrames != health->streams[1].health.endOfStreamFrames))
                    inspectionFailed.store(true);
                inspectionCount.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        const auto inspectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (inspectionCount.load() == 0 && std::chrono::steady_clock::now() < inspectionDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 14 || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 15
        const auto beforeFault = completedRealBlocks;
        faultAfterRealBlocks = beforeFault + 2;
        wholeAllocationFaultTriggered = false;
        failWholeAllocation = true;
        const auto allocationFailure = session.render(8192);
        failWholeAllocation = false;
        require(wholeAllocationFaultTriggered && completedRealBlocks >= beforeFault + 2
            && !allocationFailure.ok() && allocationFailure.mono.empty() && allocationFailure.channels.empty() && binding->active(),
            "caller allocation failure after real provisional processing restores a usable complete entry state");
#endif
        for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
            const auto failed = session.render(8448);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty() && binding->active(),
                "late public offline failure restores the real helper and returns no provisional audio");
            const auto afterAnalysis = capture->copyLatestRecordResult(0, {});
            require(afterAnalysis.status == prefixAnalysis.status && afterAnalysis.header == prefixAnalysis.header
                && capture->droppedBlockCount(0) == prefixDrops && capture->rejectedRecordCount(0) == prefixRejections,
                "real helper rollback cannot publish provisional analysis or overflow counters");
            const auto afterHealth = binding->audioStreamHealthSet();
            require(afterHealth && afterHealth->streamCount == beforeHealth->streamCount,
                "real offline rollback preserves complete resource inspection");
            for (std::size_t slot = 0; slot < afterHealth->streamCount; ++slot)
                require(afterHealth->streams[slot].seekGeneration == beforeHealth->streams[slot].seekGeneration
                    && afterHealth->streams[slot].health.discontinuities == beforeHealth->streams[slot].health.discontinuities
                    && afterHealth->streams[slot].health.endOfStreamFrames == beforeHealth->streams[slot].health.endOfStreamFrames,
                    "real failed whole calls cannot retain provisional processing health");
        }
        talas::project::ProjectDocument noiseDocument;
        noiseDocument.graph.nodes = {{"input", "talas.noise", {}}, {"out", "talas.audioOutput", {}}};
        noiseDocument.graph.connections = {{"input", "audioOut", "out", "audioIn"}};
        const auto noiseGraph = compileProject(noiseDocument, talas::core::makeBuiltinNodeRegistry(), transientRenderSettings());
        require(noiseGraph.ok(), "independent source oracle must compile");
        const auto noise = renderOffline(*noiseGraph.graph, 8192);
        require(noise.ok(), "independent source oracle must render");
        const auto retry = session.render(8192);
        require(retry.ok() && retry.mono.size() == 8192 && binding->active(),
            "the real helper must support an exact retry after repeated whole-call rollback");
        double history = 0.0;
        constexpr std::array table{0.25, 0.5, 0.75};
        constexpr std::array binary{0x00, 0x7f, 0x80, 0xfe, 0xff};
        for (std::size_t frame = 0; frame < retry.mono.size(); ++frame)
        {
            double sample = static_cast<double>(frame) / 32767.0;
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
            const auto localFrame = frame % 256;
            sample *= static_cast<double>(static_cast<float>(noise.mono[frame]))
                * (table[localFrame % table.size()] + static_cast<double>(binary[localFrame % binary.size()]) / 255.0);
#endif
            history = history * 0.5 + sample;
            const auto gain = frame + 256 < 512 ? 1.0 : 0.5;
            require(std::abs(retry.mono[frame] - static_cast<double>(static_cast<float>(history)) * gain) < 1e-6,
                "real public retry must match independent PCM, recursive state, float pipeline and scheduled gain");
        }
        const auto committedHealth = binding->audioStreamHealthSet();
        require(committedHealth && committedHealth->streamCount == beforeHealth->streamCount,
            "real commit immediately publishes complete health for its final pending block");
        for (std::size_t slot = 0; slot < committedHealth->streamCount; ++slot)
            require(committedHealth->streams[slot].health.endOfStreamFrames
                == beforeHealth->streams[slot].health.endOfStreamFrames + 256,
                "whole-call commit includes exactly one final pending EOF block per resource");
        require(!session.render(256).ok() && binding->active(),
            "late invalid input retains its original cursor after the successful retry");
        inspector.request_stop();
        inspector.join();
        require(inspectionCount.load() != 0 && !inspectionFailed.load(),
            "concurrent real public health remains complete through capture, processing, rollback and commit");
    }
    compiled.graph.reset();
    observedBinding.reset();
    binding.reset();
}
#endif

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION
void testAuthenticatedCheckpointRoundTrips(activation::ComponentActivationMacOSHost& host, bool resourceSet)
{
    using Status = activation::ComponentActivationCheckpointStatusV1;
    using Operation = activation::ComponentActivationMacOSHostOperationV1;
    using Audio = activation::ComponentActivationAudioOperationV1;
    auto control = [&](auto method, Status expected) {
        const auto before = host.heartbeatCount();
        const bool inspectHeld = (TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 1
            || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 2
            || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 10)
            && expected != Status::Fatal;
        const auto beforeHealth = inspectHeld ? host.audioStreamHealthSetForTesting() : std::nullopt;
        if (inspectHeld) host.holdNextCheckpointReplyForTesting();
        require((host.*method)() == Operation::Scheduled, "checkpoint control schedules at an exclusive boundary");
        if (inspectHeld)
        {
            const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!host.checkpointReplyHeldForTesting() && std::chrono::steady_clock::now() < heldDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(host.checkpointReplyHeldForTesting(), "real checkpoint reply reaches the held control boundary");
            const auto heldHealth = host.audioStreamHealthSetForTesting();
            require(beforeHealth && heldHealth && heldHealth->streamCount == beforeHealth->streamCount,
                "parent retains a complete public health snapshot during checkpoint control");
            for (std::size_t slot = 0; slot < heldHealth->streamCount; ++slot)
                require(heldHealth->streams[slot].seekGeneration == beforeHealth->streams[slot].seekGeneration
                    && heldHealth->streams[slot].health.endOfStreamFrames == beforeHealth->streams[slot].health.endOfStreamFrames,
                    "helper completion cannot publish provisional parent health before its acknowledgment is accepted");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            host.releaseCheckpointReplyForTesting();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (host.checkpointStatus() == Status::Pending && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (host.checkpointStatus() != expected)
            std::cerr << "checkpoint control status=" << static_cast<unsigned>(host.checkpointStatus())
                << " expected=" << static_cast<unsigned>(expected) << " state=" << static_cast<unsigned>(host.state())
                << " diagnostic=" << static_cast<unsigned>(host.diagnostic()) << '\n';
        require(host.checkpointStatus() == expected, "authenticated helper replies with the exact operation result");
        if (expected != Status::Fatal)
            require(host.state() == activation::ComponentActivationMacOSHostStateV1::PreparedActivation
                && host.heartbeatCount() > before, "checkpoint acknowledgment retains live activation and advances control identity");
    };
    constexpr bool legacy = TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 2;
    control(&activation::ComponentActivationMacOSHost::queryOfflineCheckpoint, legacy ? Status::Unsupported : Status::Supported);
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 3 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 9
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 7
    host.corruptNextCheckpointRequestForTesting(1);
    control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Fatal);
#elif TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 8
    control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
    host.corruptNextCheckpointRequestForTesting(2);
    control(&activation::ComponentActivationMacOSHost::restoreOfflineCheckpoint, Status::Fatal);
#else
    constexpr bool captureFailure = TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 3
        || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 4;
    control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, captureFailure ? Status::Fatal : Status::Captured);
    if (!captureFailure) control(&activation::ComponentActivationMacOSHost::restoreOfflineCheckpoint, Status::Fatal);
#endif
    constexpr auto expectedDiagnostic = (TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 3
        || TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 5)
        ? activation::ComponentActivationMacOSHostDiagnosticV1::DeadlineExpired
        : activation::ComponentActivationMacOSHostDiagnosticV1::ActivationRejected;
    require(host.state() == activation::ComponentActivationMacOSHostStateV1::Quarantined
        && !host.runtimeEvidence().active() && host.diagnostic() == expectedDiagnostic,
        "checkpoint terminal result is published only after exact runtime authority is revoked");
    require(!host.audioStreamHealthSetForTesting().has_value(),
        "terminal checkpoint failure bypasses every cached healthy parent snapshot");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(host.state() == activation::ComponentActivationMacOSHostStateV1::Quarantined
        && host.queryOfflineCheckpoint() == Operation::InvalidState,
        "late checkpoint completion cannot resurrect a terminated helper");
    return;
#endif
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 10
    control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::CaptureFailed);
    control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
    control(&activation::ComponentActivationMacOSHost::restoreOfflineCheckpoint, Status::Restored);
#endif
    if (legacy)
        control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Unsupported);
    std::atomic<bool> inspectionFailed{false};
    std::atomic<std::uint64_t> inspectionCount{0};
    std::array<float, 256> input{}, output{}, pending{};
    input.fill(1.0F);
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    bool hasPending = false;
    double history = 0;
    std::uint64_t generation = 0;
    auto submit = [&](std::uint64_t position, std::uint64_t epoch, bool wait = true) {
        require(host.processAudio(inputs, outputs, 256, {epoch, static_cast<std::int64_t>(position), 0, 0})
            == (hasPending ? Audio::Rendered : Audio::FirstBlockSilence), "XPC checkpoint retains logical one-block latency");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
        {
            const auto expected = hasPending ? pending[frame] : 0.0F;
            if (std::abs(output[frame] - expected) >= 1e-6F)
                std::cerr << "checkpoint XPC audio: position=" << position << " frame=" << frame
                    << " actual=" << output[frame] << " expected=" << expected << '\n';
            require(std::abs(output[frame] - expected) < 1e-6F, "real XPC rollback matches independent recursive source samples");
        }
        if (generation == 0 || generation != epoch) history = 0;
        generation = epoch;
        constexpr std::array table{0.25, 0.5, 0.75};
        constexpr std::array binary{0x00, 0x7f, 0x80, 0xfe, 0xff};
        for (std::size_t frame = 0; frame < pending.size(); ++frame)
        {
            auto sample = position + frame < 8192
                ? static_cast<double>(position + frame) / 32767.0 : 0.0;
            if (resourceSet) sample *= table[frame % table.size()] + static_cast<double>(binary[frame % binary.size()]) / 255.0;
            history = (legacy ? 0 : history * 0.5) + sample;
            pending[frame] = static_cast<float>(history);
        }
        hasPending = true;
        if (wait) require(waitForPublishedAudioResponse([&] { return host.publishedAudioResponseReadyForTesting(); }),
            "helper publishes the exact checkpoint test request");
    };
    auto health = [&] {
        auto observed = host.audioStreamHealthSetForTesting();
        require(observed.has_value() && observed->streamCount == (resourceSet ? 2U : 1U),
            "checkpoint acknowledgment exposes every authenticated resource health slot");
        return *observed;
    };
    auto sameProcessingHealth = [&](const auto& expected) {
        const auto observed = health();
        for (std::size_t slot = 0; slot < observed.streamCount; ++slot)
            require(observed.streams[slot].seekGeneration == expected.streams[slot].seekGeneration
                && observed.streams[slot].health.discontinuities == expected.streams[slot].health.discontinuities
                && observed.streams[slot].health.endOfStreamFrames == expected.streams[slot].health.endOfStreamFrames,
                "provisional EOF and seek health remain captured and restore without another audio block");
    };
    std::jthread inspector([&](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            const auto observed = host.audioStreamHealthSetForTesting();
            if (!observed || observed->streamCount != (resourceSet ? 2U : 1U))
                inspectionFailed.store(true, std::memory_order_relaxed);
            else if (resourceSet)
            {
                const auto& first = observed->streams[0];
                const auto& second = observed->streams[1];
                if (first.seekGeneration != second.seekGeneration
                    || first.health.discontinuities != second.health.discontinuities
                    || first.health.endOfStreamFrames != second.health.endOfStreamFrames)
                    inspectionFailed.store(true, std::memory_order_relaxed);
            }
            inspectionCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    if (!legacy)
    {
        for (std::uint64_t attempt = 0; attempt < 8; ++attempt)
        {
            const auto savedHistory = history;
            const auto savedGeneration = generation;
            const auto savedPending = pending;
            const auto savedHasPending = hasPending;
            control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
            require(host.beginOfflineCheckpoint() == Operation::InvalidState, "host rejects nested checkpoint before sending another command");
            const auto capturedHealth = health();
            if (attempt == 0) control(&activation::ComponentActivationMacOSHost::queryOfflineCheckpoint, Status::Supported);
            for (std::uint64_t block = 0; block < 4; ++block)
                submit(block == 3 ? 8192 : attempt * 512 + block * 256, 9, block != 3);
            sameProcessingHealth(capturedHealth);
            control(&activation::ComponentActivationMacOSHost::restoreOfflineCheckpoint, Status::Restored);
            sameProcessingHealth(capturedHealth);
            history = savedHistory; generation = savedGeneration; pending = savedPending; hasPending = savedHasPending;
            submit(attempt * 512, 7); submit(attempt * 512 + 256, 7, false);
        }
        control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
        const auto beforeCommitHealth = health();
        submit(8192, 11, false);
        control(&activation::ComponentActivationMacOSHost::commitOfflineCheckpoint, Status::Committed);
        const auto committedHealth = health();
        for (std::size_t slot = 0; slot < committedHealth.streamCount; ++slot)
            require(committedHealth.streams[slot].health.endOfStreamFrames
                    == beforeCommitHealth.streams[slot].health.endOfStreamFrames + 256
                && committedHealth.streams[slot].health.discontinuities
                    == beforeCommitHealth.streams[slot].health.discontinuities + 1
                && committedHealth.streams[slot].seekGeneration == beforeCommitHealth.streams[slot].seekGeneration + 1,
                "commit acknowledgment publishes exactly one provisional EOF block and seek for each resource");
        submit(8448, 11); submit(8704, 11);
        require(host.restoreOfflineCheckpoint() == Operation::InvalidState, "committed helper checkpoint identity is consumed");
        control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
        const auto beforeIdle = host.heartbeatCount();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto idleHeartbeats = host.heartbeatCount() - beforeIdle;
        require(idleHeartbeats >= 1 && idleHeartbeats <= 4,
            "checkpoint replies retain one heartbeat timer chain while a snapshot is held");
        // Caller teardown below must retire the held helper and host snapshots.
    }
    else { submit(0, 7); submit(256, 7); }
    inspector.request_stop();
    inspector.join();
    require(inspectionCount.load(std::memory_order_relaxed) > 0
        && !inspectionFailed.load(std::memory_order_relaxed),
        "concurrent parent inspection retains complete healthy resource rounds through control and provisional audio");
    if (resourceSet)
    {
        control(&activation::ComponentActivationMacOSHost::commitOfflineCheckpoint, Status::Committed);
        const auto beforePartialRead = health();
        std::optional<talas::runtime::IsolatedComponentAudioStreamHealthSetSnapshot> partialRead;
        host.holdNextResourceSetHealthReadForTesting();
        std::jthread oneRead([&] { partialRead = host.audioStreamHealthSetForTesting(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!host.resourceSetHealthReadHeldForTesting() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(host.resourceSetHealthReadHeldForTesting(), "resource-set inspector pauses after accepting only the first slot");
        submit(8192, 6);
        host.releaseResourceSetHealthReadForTesting();
        oneRead.join();
        require(partialRead && partialRead->streamCount == 2,
            "partial resource publication retains an existing complete parent snapshot");
        for (std::size_t slot = 0; slot < 2; ++slot)
            require(partialRead->streams[slot].health.endOfStreamFrames == beforePartialRead.streams[slot].health.endOfStreamFrames,
                "one old slot and one new slot cannot form a public resource-set snapshot");
        const auto nextComplete = health();
        for (std::size_t slot = 0; slot < 2; ++slot)
            require(nextComplete.streams[slot].health.endOfStreamFrames == beforePartialRead.streams[slot].health.endOfStreamFrames + 256,
                "the next complete resource round advances both public slots together");
        control(&activation::ComponentActivationMacOSHost::beginOfflineCheckpoint, Status::Captured);
    }
}
#endif

void testSealedRuntimeLifecycle()
{
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    constexpr auto profile =
        activation::ComponentActivationMacOSProfileV1::BoundedAudioStream;
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 1
    constexpr auto profile = activation::ComponentActivationMacOSProfileV1::
        MultipleImmutableOnlyResources;
#else
    constexpr auto profile = activation::ComponentActivationMacOSProfileV1::
        MultipleImmutableResources;
#endif
#else
    constexpr auto profile =
        (TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13
         || TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14
         || TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15
         || TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16)
            ? activation::ComponentActivationMacOSProfileV1::ImmutableResource
            : activation::ComponentActivationMacOSProfileV1::StatelessAudio;
#endif
    auto prepared = activation::ComponentActivationMacOSHost::prepare(
        endpoint(),
        profile);
    require(
        prepared.ok()
            && prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Prepared
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::Ready
            && activation::validateComponentActivationSealedSessionV1(
                   prepared.host->sealedSession())
                == activation::ComponentActivationProtocolDiagnosticV1::Valid
            && prepared.host->sealedStorageContainsNoComponentBytes()
            && !prepared.host->runtimeEvidence().active(),
        "preparation should own one pristine sealed session without evidence");
    require(
        prepared.host->start()
            == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "the sealed runtime launch should schedule exactly once");

#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 1 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 7 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 8 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 9 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 11 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 12 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Ready),
        "the production sealed helper should become ready");
    auto evidence = prepared.host->runtimeEvidence();
    require(
        evidence.active() && prepared.host->peerProcessIdentifier() > 0,
        "ready state should expose only live host-authored runtime evidence");
    const auto heartbeatDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (prepared.host->heartbeatCount() < 2
        && std::chrono::steady_clock::now() < heartbeatDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(
        prepared.host->heartbeatCount() >= 2 && evidence.active(),
        "the exact peer should keep evidence live through bounded heartbeats");

    const auto peer = prepared.host->peerProcessIdentifier();
    ActivationAuthorityFixture activationFixture{*prepared.host};
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION == 1
    prepared.host->overlapNextActivationHeartbeatForTesting();
#endif
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 1
    const auto initialMaterializationCount = activationMaterializationCount();
    auto mismatchedLease = activationFixture.lease(83);
    require(
        mismatchedLease.has_value(),
        "exact evidence should issue a mismatch-test lease");
    const auto changedParameters = gainParameters(0.75);
    require(
        prepared.host->activate(
            std::move(*mismatchedLease),
            activationFixture.snapshot,
            changedParameters)
            == activation::ComponentActivationMacOSHostOperationV1::Rejected
            && prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Ready,
        "changed parameters must reject before component bytes are sent");

    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one exact live authority should schedule one frozen-byte activation");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::PreparedActivation)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::Ready
            && evidence.active(),
        "the exact gain module should load, create, and prepare in the helper");
    require(
        !peerHoldsWritableMaterialization(peer),
        "the helper must not keep a writable descriptor to the host's image file");
    constexpr auto profileChannels = static_cast<std::size_t>(
        TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS);
    constexpr auto profileBlockFrames = static_cast<std::size_t>(
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES);
    std::array<std::array<float, 512>, 2> audioInput{};
    std::array<std::array<float, 512>, 2> audioOutput{};
    std::array<const float*, 2> audioInputs{};
    std::array<float*, 2> audioOutputs{};
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        std::fill_n(
            audioInput[channel].begin(),
            profileBlockFrames,
            static_cast<float>(channel + 1U));
        std::fill_n(audioOutput[channel].begin(), profileBlockFrames, 9.0F);
        audioInputs[channel] = audioInput[channel].data();
        audioOutputs[channel] = audioOutput[channel].data();
    }
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1;
    const auto firstOperation = prepared.host->processAudio(
        {audioInputs.data(), profileChannels},
        {audioOutputs.data(), profileChannels},
        profileBlockFrames,
        transport);
    auto firstSilent = true;
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        firstSilent = firstSilent
            && std::all_of(
                audioOutput[channel].begin(),
                audioOutput[channel].begin() + profileBlockFrames,
                [](float value) { return value == 0.0F; });
    }
    require(
        firstOperation
                == activation::ComponentActivationAudioOperationV1::
                    FirstBlockSilence
            && firstSilent,
        "callback one must publish without entering the helper from the host thread");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "the helper should publish response one before callback two is tested");
    transport.sourceFrame = TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    const auto secondOperation = prepared.host->processAudio(
        {audioInputs.data(), profileChannels},
        {audioOutputs.data(), profileChannels},
        profileBlockFrames,
        transport);
    auto secondExact = true;
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        const auto expected = static_cast<float>(channel + 1U) * 0.5F;
        secondExact = secondExact
            && std::all_of(
                audioOutput[channel].begin(),
                audioOutput[channel].begin() + profileBlockFrames,
                [expected](float value) { return value == expected; });
    }
    require(
        secondOperation
                == activation::ComponentActivationAudioOperationV1::Rendered
            && secondExact,
        "callback two must consume exactly one delayed helper-rendered gain block");
    const auto preparedHeartbeat = prepared.host->heartbeatCount();
    const auto preparedHeartbeatDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (prepared.host->heartbeatCount() <= preparedHeartbeat
        && std::chrono::steady_clock::now() < preparedHeartbeatDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(
        prepared.host->heartbeatCount() > preparedHeartbeat
            && evidence.active(),
        "prepared authorization should remain bound to live heartbeats");
    require(
        waitForMaterializationCount(initialMaterializationCount),
        "successful activation must remove its private materialization path");

    auto replayLease = activationFixture.lease(85);
    require(
        replayLease.has_value()
            && prepared.host->activate(
                std::move(*replayLease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::InvalidState,
        "the same sealed helper must reject a second authorization");
    prepared.host->shutdown();
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Closed
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::Closed
            && !evidence.active(),
        "shutdown must revoke runtime evidence before closing storage");
    require(
        peerHasExited(peer),
        "Closed must not publish before the exact helper process exits");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 13
    auto lease = activationFixture.lease(84);
    const auto resourceActivation = lease.has_value()
        ? prepared.host->activate(
              std::move(*lease),
              activationFixture.snapshot,
              activationFixture.parameters)
        : activation::ComponentActivationMacOSHostOperationV1::Rejected;
    require(
        lease.has_value()
            && resourceActivation
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one exact immutable-table authority should schedule activation");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::PreparedActivation)
            && evidence.active(),
        "the helper should map, resolve, create, and prepare the resource fixture");

    constexpr std::size_t frames = 256;
    std::array<float, frames> audioInput{};
    std::array<float, frames> audioOutput{};
    audioInput.fill(1.0F);
    const std::array<const float*, 1> audioInputs{audioInput.data()};
    const std::array<float*, 1> audioOutputs{audioOutput.data()};
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1;
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REVOKE_BEFORE_READ
    const auto denied = activationFixture.owner.replaceOwnerAction(
        activationFixture.candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        activationFixture.owner.generation());
    require(denied.ok(),
        "owner denial should revoke the complete active resource set");
    audioOutput.fill(1.0F);
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::Quarantined
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "resource-set revoke-before-read must publish bounded silence");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && !evidence.active(),
        "resource-set revocation must withdraw evidence and quarantine the host");
    prepared.host->shutdown();
    require(
        waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "resource-set revocation must leave no mapping or helper residue");
    return;
#endif
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::
                    FirstBlockSilence
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "resource callback one should remain bounded first-block silence");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "the helper should publish the immutable-table response before callback two");
    transport.sourceFrame = frames;
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::Rendered,
        "resource callback two should consume the helper-rendered block");
    constexpr std::array expected{0.25F, 0.5F, 0.75F};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(
            audioOutput[frame] == expected[frame % expected.size()],
            "process should read the retained immutable table pointer directly");
    }
    prepared.host->shutdown();
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Closed
            && !evidence.active() && peerHasExited(peer),
        "resource mapping teardown should revoke evidence and close the exact peer");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 15
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    const auto binaryActivation = lease.has_value()
        ? prepared.host->activate(
              std::move(*lease),
              activationFixture.snapshot,
              activationFixture.parameters)
        : activation::ComponentActivationMacOSHostOperationV1::Rejected;
    require(
        lease.has_value()
            && binaryActivation
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one exact immutable-binary authority should schedule validation");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::PreparedActivation)
            && evidence.active(),
        "the helper should map, resolve, create, and prepare the binary fixture");

    constexpr std::size_t frames = 256;
    std::array<float, frames> audioInput{};
    std::array<float, frames> audioOutput{};
    audioInput.fill(1.0F);
    const std::array<const float*, 1> audioInputs{audioInput.data()};
    const std::array<float*, 1> audioOutputs{audioOutput.data()};
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1;
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::
                    FirstBlockSilence
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "binary callback one should remain bounded first-block silence");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "the helper should publish the immutable-binary response before callback two");
    transport.sourceFrame = frames;
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::Rendered,
        "binary callback two should consume the helper-rendered block");
    constexpr std::array expected{0.0F, 127.0F, 128.0F, 254.0F, 255.0F};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(
            audioOutput[frame] == expected[frame % expected.size()],
            "process should read every retained immutable binary octet exactly");
    }

    prepared.host->shutdown();
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Closed
            && !evidence.active() && peerHasExited(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "binary validation teardown should revoke evidence, close the exact peer, and remove materialization");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 18
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    const auto setActivation = lease.has_value()
        ? prepared.host->activate(
              std::move(*lease),
              activationFixture.snapshot,
              activationFixture.parameters)
        : activation::ComponentActivationMacOSHostOperationV1::Rejected;
    if (!lease.has_value()
        || setActivation
            != activation::ComponentActivationMacOSHostOperationV1::Scheduled)
    {
        std::cerr << "resource-set admission: lease=" << lease.has_value()
                  << " operation="
                  << static_cast<unsigned>(setActivation)
                  << " host-state="
                  << static_cast<unsigned>(prepared.host->state())
                  << " diagnostic="
                  << static_cast<unsigned>(prepared.host->diagnostic())
                  << '\n';
    }
    require(
        lease.has_value()
            && setActivation
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one complete mixed resource set should schedule sealed XPC validation");
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    ActivationRejected
            && !evidence.active()
            && waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "partial resource-set resolution must reject the complete activation");
#else
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::PreparedActivation)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::Ready
            && evidence.active(),
        "the product Runtime helper should resolve and prepare the complete set");

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 14 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 22
    testRealBindingOfflineTransactions(*prepared.host, activationFixture);
#else
    testAuthenticatedCheckpointRoundTrips(*prepared.host, true);
#endif
    prepared.host->shutdown();
    require(!evidence.active() && waitForPeerExit(peer)
        && waitForMaterializationCount(initialMaterializationCount),
        "checkpoint integration revokes evidence, retires its exact peer and removes materialized image");
    return;
#endif


#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH
    const auto document = transientComponentDocument(
        activationFixture.snapshot);
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge ordinary(document, registry);
    require(
        ordinary.compileAndStage(
                    transientResourceSetRenderSettings(
                        activationFixture.resourceSetSnapshots))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "ordinary Open/Play must keep the aggregate capsule preserve-only");

    auto binding = std::move(*prepared.host)
                       .takePreparedIsolatedComponentBinding(
                           "component",
                           activationFixture.snapshot,
                           activationFixture.parameters);
    require(
        binding != nullptr && binding->active()
            && binding->contract().resources.has_value()
            && binding->contract().resources->grant.has_value()
            && binding->contract().resources->members.size()
                == resourceSetFixtureMembers.size()
            && binding->contract().resources->memberRecords.size()
                == resourceSetFixtureMembers.size()
            && binding->contract().resources->grant->memberCount
                == resourceSetFixtureMembers.size(),
        "the Host must author one exact ordered aggregate graph contract");

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    const auto aggregateStage = bridge.compileAndStageIsolatedComponent(
        document,
        transientResourceSetRenderSettings(
            activationFixture.resourceSetSnapshots),
        binding);
    if (!aggregateStage.ok())
    {
        std::cerr << "aggregate graph stage failed: outcome="
                  << static_cast<unsigned>(aggregateStage.outcome)
                  << " message=" << aggregateStage.message << '\n';
        for (const auto& diagnostic : aggregateStage.diagnostics)
        {
            std::cerr << "  node=" << diagnostic.nodeId
                      << " parameter=" << diagnostic.parameterId
                      << " message=" << diagnostic.message << '\n';
        }
    }
    require(
        aggregateStage.ok(),
        "the exact signed aggregate capsule should stage atomically");
    constexpr std::size_t frames = 256;
    std::array<float, frames> audioInput{};
    std::array<float, frames> audioOutput{};
    audioInput.fill(1.0F);
    const std::array<const float*, 1> audioInputs{audioInput.data()};
    const talas::runtime::RealtimeAudioInputBlock graphInput{
        audioInputs.data(), audioInputs.size()};
    const std::array<float*, 1> audioOutputs{audioOutput.data()};
    require(
        bridge.render(
            graphInput,
            audioOutputs.data(),
            audioOutputs.size(),
            frames)
            && std::ranges::all_of(audioOutput, [](float value) {
                   return value == 0.0F;
               }),
        "the first aggregate graph callback should adopt and remain silent");
    require(
        waitForPublishedAudioResponse([&] {
            return binding->publishedAudioResponseReadyForTesting();
        }),
        "the aggregate Runtime should publish callback one before graph callback two");
    require(
        bridge.render(
            graphInput,
            audioOutputs.data(),
            audioOutputs.size(),
            frames),
        "the second aggregate graph callback should consume the complete response");
    constexpr std::array tableValues{0.25, 0.5, 0.75};
    constexpr std::array binaryValues{0x00, 0x7f, 0x80, 0xfe, 0xff};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto sourceSample = static_cast<double>(frame) / 32'767.0;
        const auto binaryScale = static_cast<double>(
            binaryValues[frame % binaryValues.size()]) / 255.0;
        double expected = 0.0;
        for (const auto& member : resourceSetFixtureMembers)
        {
            if (member.kind == ResourceSetFixtureKind::Table)
                expected += tableValues[frame % tableValues.size()];
            else if (member.kind == ResourceSetFixtureKind::Binary)
                expected += binaryScale;
            else
                expected += sourceSample;
        }
        require(
            std::abs(static_cast<double>(audioOutput[frame]) - expected)
                < 1.0e-6,
            "the active graph must retain and consume every signed set member");
    }
    const auto health = bridge.streamHealth();
    require(
        health.consumerCount
                == binding->contract().resources->grant->audioStreamCount
            && health.consumers.size() == health.consumerCount,
        "aggregate graph health must expose each Audio slot exactly once");
    for (const auto& snapshot : activationFixture.resourceSetSnapshots)
    {
        const auto accepted = bridge.acceptedResourceIdentity(snapshot->id);
        require(
            accepted.has_value()
                && accepted->kind == snapshot->kind
                && accepted->generation == snapshot->generation
                && accepted->contentHash == snapshot->contentHash,
            "aggregate graph adoption must publish all member histories together");
    }
    require(
        talas::project::serializeProjectDocument(document) == canonicalBefore,
        "aggregate activation must not mutate the canonical project document");

    binding.reset();
    require(
        bridge.compileAndStageDocument(
                  directAudioDocument(), transientRenderSettings())
                .ok()
            && bridge.render(
                graphInput,
                audioOutputs.data(),
                audioOutputs.size(),
                frames)
            && bridge.collectRetired(),
        "a built-in graph should replace and reclaim the aggregate graph off callback");
    require(
        !evidence.active() && waitForPeerExit(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "aggregate graph reclamation must close the exact peer without residue");
#else
    constexpr std::size_t frames = 256;
    std::array<float, frames> audioInput{};
    std::array<float, frames> audioOutput{};
    audioInput.fill(1.0F);
    const std::array<const float*, 1> audioInputs{audioInput.data()};
    const std::array<float*, 1> audioOutputs{audioOutput.data()};
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1;
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::
                    FirstBlockSilence,
        "mixed-resource callback one should remain bounded first-block silence");
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE != 0
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    audioOutput.fill(9.0F);
    transport.sourceFrame = frames;
    const auto failedOperation = prepared.host->processAudio(
        audioInputs, audioOutputs, frames, transport);
    require(
        failedOperation
                != activation::ComponentActivationAudioOperationV1::Rendered
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "a stalled set worker must return bounded silence on the next callback");
    const auto failureQuarantined = waitForTerminalState(
        *prepared.host,
        activation::ComponentActivationMacOSHostStateV1::Quarantined);
    const auto expectedFailureDiagnostic =
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE == 2
        ? activation::ComponentActivationMacOSHostDiagnosticV1::DeadlineExpired
        : activation::ComponentActivationMacOSHostDiagnosticV1::
            ActivationRejected;
    if (!failureQuarantined
        || prepared.host->diagnostic() != expectedFailureDiagnostic)
    {
        std::cerr << "resource-set runtime failure: state="
                  << static_cast<unsigned>(prepared.host->state())
                  << " diagnostic="
                  << static_cast<unsigned>(prepared.host->diagnostic())
                  << " expected="
                  << static_cast<unsigned>(expectedFailureDiagnostic)
                  << " evidence=" << evidence.active() << '\n';
    }
    require(
        failureQuarantined
            && prepared.host->diagnostic() == expectedFailureDiagnostic
            && !evidence.active()
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                })
            && waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "a crashing or hanging set worker must quarantine the complete activation");
#else
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "the helper should publish the mixed-resource response before callback two");
    transport.sourceFrame = frames;
    require(
        prepared.host->processAudio(
            audioInputs, audioOutputs, frames, transport)
                == activation::ComponentActivationAudioOperationV1::Rendered,
        "mixed-resource callback two should consume the helper-rendered block");
    constexpr std::array tableValues{0.25, 0.5, 0.75};
    constexpr std::array binaryValues{0x00, 0x7f, 0x80, 0xfe, 0xff};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto sourceSample = static_cast<double>(frame) / 32'767.0;
        const auto binaryScale = static_cast<double>(
            binaryValues[frame % binaryValues.size()]) / 255.0;
#if TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE == 0
        const auto expected = sourceSample
            * (tableValues[frame % tableValues.size()] + binaryScale);
#else
        double expected = 0.0;
        for (const auto& member : resourceSetFixtureMembers)
        {
            if (member.kind == ResourceSetFixtureKind::Table)
                expected += tableValues[frame % tableValues.size()];
            else if (member.kind == ResourceSetFixtureKind::Binary)
                expected += binaryScale;
            else
                expected += sourceSample;
        }
#endif
        require(
            std::abs(static_cast<double>(audioOutput[frame]) - expected)
                < 1.0e-6,
            "mixed-resource process should consume every prepared member exactly");
    }

    prepared.host->shutdown();
    const auto closedAfterSetShutdown = prepared.host->state()
        == activation::ComponentActivationMacOSHostStateV1::Closed;
    const auto evidenceRevokedAfterSetShutdown = !evidence.active();
    const auto materializationRemovedAfterSetShutdown =
        waitForMaterializationCount(initialMaterializationCount);
    const auto peerExitedAfterSetShutdown = waitForPeerExit(peer);
    if (!closedAfterSetShutdown || !evidenceRevokedAfterSetShutdown
        || !peerExitedAfterSetShutdown || !materializationRemovedAfterSetShutdown)
    {
        std::cerr << "resource-set shutdown: closed="
                  << closedAfterSetShutdown
                  << " evidence-revoked="
                  << evidenceRevokedAfterSetShutdown
                  << " peer-exited="
                  << peerExitedAfterSetShutdown
                  << " materialization-removed="
                  << materializationRemovedAfterSetShutdown << '\n';
    }
    require(
        closedAfterSetShutdown && evidenceRevokedAfterSetShutdown
            && peerExitedAfterSetShutdown
            && materializationRemovedAfterSetShutdown,
        "mixed-resource teardown should revoke evidence and close the exact peer");
#endif
#endif
#endif
#else
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    ActivationRejected
            && !evidence.active()
            && waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "the product helper should privately materialize the complete set before the incompatible fixture is rejected");
#endif
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 17
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    const auto audioActivation = lease.has_value()
        ? prepared.host->activate(
              std::move(*lease),
              activationFixture.snapshot,
              activationFixture.parameters)
        : activation::ComponentActivationMacOSHostOperationV1::Rejected;
    require(
        lease.has_value()
            && audioActivation
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one exact frozen Audio authority should schedule Runtime activation");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::PreparedActivation)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::Ready
            && evidence.active(),
        "pathless Audio should resolve, prewarm, pass canary/reset, and become active");

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 14 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 22
    testRealBindingOfflineTransactions(*prepared.host, activationFixture);
#else
    testAuthenticatedCheckpointRoundTrips(*prepared.host, false);
#endif
    prepared.host->shutdown();
    require(!evidence.active() && waitForPeerExit(peer)
        && waitForMaterializationCount(initialMaterializationCount),
        "checkpoint integration revokes evidence, retires its exact peer and removes materialized image");
    return;
#endif


#if TALAS_COMPONENT_ACTIVATION_AUDIO_CANCEL_ACTIVE_PEER
    prepared.host->cancelConnectionForTesting();
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && !evidence.active() && waitForPeerExit(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "disconnecting an active Runtime peer must stop its worker before finalizing shared Audio storage");
    prepared.host->shutdown();
    require(
        prepared.host->state()
            == activation::ComponentActivationMacOSHostStateV1::Quarantined,
        "a disconnected active Runtime peer must remain fail-closed after shutdown");
    return;
#endif

#if TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH
    const auto document = transientComponentDocument(
        activationFixture.snapshot);
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto resourceSnapshot = activationFixture.inputs.resourceSnapshot;
    require(
        resourceSnapshot != nullptr,
        "active Audio graph fixture should retain its immutable metadata snapshot");
    talas::runtime::ProjectRuntimeBridge ordinary(document, registry);
    require(
        ordinary.compileAndStage(
                    transientRenderSettings(resourceSnapshot))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "ordinary Open/Play must keep the Audio capsule preserve-only");

    auto binding = std::move(*prepared.host)
                       .takePreparedIsolatedComponentBinding(
                           "component",
                           activationFixture.snapshot,
                           activationFixture.parameters);
    const auto* audioContract = binding != nullptr
            && binding->contract().resources.has_value()
            && binding->contract().resources->members.size() == 1
        ? std::get_if<talas::runtime::IsolatedComponentAudioStreamContract>(
            &binding->contract().resources->members.front())
        : nullptr;
    require(
        binding != nullptr && binding->active()
            && audioContract != nullptr
            && audioContract->parameterId == "audio"
            && audioContract->resourceId
                == resourceSnapshot->id
            && audioContract->generation
                == resourceSnapshot->generation
            && audioContract->sourceContentHash
                == resourceSnapshot->contentHash
            && audioContract->sourceByteCount
                == 44U + 8'192U * TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS * 2U
            && audioContract->sourceFrameCount
                == resourceSnapshot->frameCount
            && audioContract->sourceSampleRate
                == resourceSnapshot->sampleRate
            && audioContract->sourceChannelCount
                == resourceSnapshot->channelCount
            && audioContract->streamProfileVersion
                == activation::componentActivationAudioStreamProfileVersionV1
            && audioContract->sourceEncoding
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationAudioSourceEncodingV1::
                        Pcm16Wave),
        "the prepared Host must author one exact pathless Audio graph contract");

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  transientRenderSettings(resourceSnapshot),
                  binding)
                .ok(),
        "the exact Audio capsule should stage only with its live Host binding");
    constexpr auto graphFrames = static_cast<std::size_t>(
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES);
    std::array<float, 512> graphInput{};
    std::array<float, 512> graphOutput{};
    const std::array<const float*, 1> graphInputs{graphInput.data()};
    const talas::runtime::RealtimeAudioInputBlock graphInputBlock{
        graphInputs.data(), graphInputs.size()};
    std::array<float*, 1> graphOutputs{graphOutput.data()};
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            graphFrames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + graphFrames,
                [](float value) { return value == 0.0F; }),
        "the first Audio graph callback should adopt and remain silent");
#if TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE != 0
    const auto healthFailureDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
#if TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE == 3
    // A helper can quarantine its shared plane between callbacks. Deliberately
    // make no further callback: revocation must not depend on a parent latch.
    while (binding->active() && std::chrono::steady_clock::now() < healthFailureDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!binding->active(), "helper health failure quarantines the plane without another audio callback");
#else
    bool healthCallbacksFailedClosed = true;
    while (binding->active()
        && std::chrono::steady_clock::now() < healthFailureDeadline)
    {
        graphOutput.fill(1.0F);
        healthCallbacksFailedClosed = healthCallbacksFailedClosed
            && bridge.render(
                graphInputBlock,
                graphOutputs.data(),
                graphOutputs.size(),
                graphFrames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + graphFrames,
                [](float value) { return value == 0.0F; });
        if (!healthCallbacksFailedClosed)
            break;
        std::this_thread::yield();
    }
    if (!healthCallbacksFailedClosed || binding->active())
    {
        std::cerr << "Audio health graph state: callbacksFailedClosed="
                  << healthCallbacksFailedClosed
                  << " bindingActive=" << binding->active()
                  << " firstOutput=" << graphOutput.front() << '\n';
    }
    require(
        healthCallbacksFailedClosed && !binding->active(),
        "stream worker health failure must revoke and silence the active graph");
#endif
    const auto evidenceRevocationDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (evidence.active()
        && std::chrono::steady_clock::now() < evidenceRevocationDeadline)
    {
        std::this_thread::yield();
    }
    require(
        !evidence.active(),
        "the control plane must revoke Audio runtime evidence after helper failure even without another callback");
    graphOutput.fill(1.0F);
    require(bridge.render(graphInputBlock, graphOutputs.data(), graphOutputs.size(), graphFrames)
        && std::all_of(graphOutput.begin(), graphOutput.begin() + graphFrames, [](float value) { return value == 0.0F; }),
        "callbacks resumed after helper failure remain silent");
#else
    require(
        waitForPublishedAudioResponse([&] {
            return binding->publishedAudioResponseReadyForTesting();
        }),
        "the Runtime stream should publish callback one before graph callback two");
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            graphFrames),
        "the second Audio graph callback should consume the stream response");
    for (std::size_t frame = 0; frame < graphFrames; ++frame)
    {
        require(
            std::abs(
                graphOutput[frame]
                - static_cast<float>(frame) / 32'767.0F) < 1.0e-6F,
            "the active graph should read the exact retained pathless Audio source");
    }
    const auto streamHealth = bridge.streamHealth();
    require(
        streamHealth.consumerCount == 1
            && streamHealth.consumers.size() == 1
            && streamHealth.consumers.front().structuralSourcePath.empty()
            && streamHealth.consumers.front().resourceId
                == resourceSnapshot->id
            && streamHealth.consumers.front().seekGeneration != 0U
            && streamHealth.consumers.front().health.bufferCapacity
                == activation::componentActivationAudioResponseCapacityV1
            && streamHealth.consumers.front().health.lowWatermark
                == activation::componentActivationAudioLowWatermarkBlocksV1
            && streamHealth.consumers.front().health.highWatermark
                == activation::componentActivationAudioHighWatermarkBlocksV1
            && streamHealth.totals.decodedBlocks != 0U
            && streamHealth.totals.decodeFailures == 0U,
        "the graph health aggregate should consume the authenticated pathless Audio health plane");
#endif
    binding.reset();
    const auto accepted = bridge.acceptedResourceIdentity(resourceSnapshot->id);
    require(
        accepted.has_value()
            && accepted->kind == talas::core::ResourceSnapshotKind::Audio
            && accepted->generation == resourceSnapshot->generation
            && accepted->contentHash == resourceSnapshot->contentHash
            && registry.findNodeType(
                   activationFixture.snapshot.manifest.nodes.front().typeId)
                == nullptr
            && talas::project::serializeProjectDocument(document)
                == canonicalBefore,
        "Audio staging must record exact history without canonical mutation");
    require(
        bridge.compileAndStageDocument(
                  directAudioDocument(), transientRenderSettings())
                .ok()
            && bridge.render(
                graphInputBlock,
                graphOutputs.data(),
                graphOutputs.size(),
                graphFrames)
            && bridge.collectRetired(),
        "a built-in graph should replace and retire the active Audio graph");
    require(
        !evidence.active() && waitForPeerExit(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "Audio graph retirement must stop its worker, mapping, and helper peer");
    return;
#endif

#if TALAS_COMPONENT_ACTIVATION_AUDIO_PARTITION_FRAMES != 0
    constexpr std::size_t partitionOuterFrames =
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    constexpr std::size_t partitionReadFrames =
        TALAS_COMPONENT_ACTIVATION_AUDIO_PARTITION_FRAMES;
    std::array<float, 512> partitionInput{};
    std::array<float, 512> partitionOutput{};
    partitionOutput.fill(1.0F);
    const std::array<const float*, 1> partitionInputs{partitionInput.data()};
    const std::array<float*, 1> partitionOutputs{partitionOutput.data()};
    activation::ComponentActivationAudioTransportV1 partitionTransport;
    partitionTransport.generation = 1U;
    require(
        prepared.host->processAudio(
            partitionInputs,
            partitionOutputs,
            partitionOuterFrames,
            partitionTransport)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "partitioned stream callback one should publish without waiting");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the partitioned stream response");
    partitionTransport.sourceFrame = partitionOuterFrames;
    require(
        prepared.host->processAudio(
            partitionInputs,
            partitionOutputs,
            partitionOuterFrames,
            partitionTransport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "outer callback should consume the one-call partitioned response");
    for (std::size_t frame = 0; frame < partitionOuterFrames; ++frame)
    {
        const auto expected = frame < partitionReadFrames
            ? static_cast<float>(frame) / 32'767.0F
            : 0.0F;
        require(
            std::abs(partitionOutput[frame] - expected) < 1.0e-6F,
            "1/7-frame read should preserve its exact prefix and zero remainder");
    }
    prepared.host->shutdown();
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Closed
            && !evidence.active()
            && waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "partitioned Audio teardown must leave no Runtime residue");
    return;
#endif

#if TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE != 0
    constexpr std::size_t failureFrames =
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    std::array<float, 512> failureInput{};
    std::array<float, 512> failureOutput{};
    failureOutput.fill(1.0F);
    const std::array<const float*, 1> failureInputs{failureInput.data()};
    const std::array<float*, 1> failureOutputs{failureOutput.data()};
    activation::ComponentActivationAudioTransportV1 failureTransport;
    failureTransport.generation = 1U;
    require(
        prepared.host->processAudio(
            failureInputs,
            failureOutputs,
            failureFrames,
            failureTransport)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "failing Audio process should first publish without waiting");
    const auto failureDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(100);
    while (prepared.host->state()
            != activation::ComponentActivationMacOSHostStateV1::Quarantined
        && !prepared.host->publishedAudioResponseReadyForTesting()
        && std::chrono::steady_clock::now() < failureDeadline)
    {
        std::this_thread::yield();
    }
    failureTransport.sourceFrame = failureFrames;
    const auto failureOperation = prepared.host->processAudio(
            failureInputs,
            failureOutputs,
            failureFrames,
            failureTransport);
    require(
        failureOperation
                == (TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE == 2
                        ? activation::ComponentActivationAudioOperationV1::
                            Missing
                        : activation::ComponentActivationAudioOperationV1::
                            Quarantined)
            && std::all_of(
                failureOutput.begin(),
                failureOutput.begin() + failureFrames,
                [](float value) { return value == 0.0F; }),
        "crash, hang, or malformed Audio ABI must fail closed as silence");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && !evidence.active(),
        "Audio Runtime failure must quarantine and revoke evidence");
    prepared.host->shutdown();
    require(
        waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "Audio Runtime failure must leave no materialization or helper process");
    return;
#endif

#if TALAS_COMPONENT_ACTIVATION_AUDIO_REVOKE_BEFORE_READ
    const auto denied = activationFixture.owner.replaceOwnerAction(
        activationFixture.candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        activationFixture.owner.generation());
    require(denied.ok(), "owner denial should revoke the active Audio lease");
    constexpr std::size_t revokedFrames =
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    std::array<float, 512> revokedInput{};
    std::array<float, 512> revokedOutput{};
    revokedOutput.fill(1.0F);
    const std::array<const float*, 1> revokedInputs{revokedInput.data()};
    const std::array<float*, 1> revokedOutputs{revokedOutput.data()};
    activation::ComponentActivationAudioTransportV1 revokedTransport;
    revokedTransport.generation = 1U;
    require(
        prepared.host->processAudio(
            revokedInputs,
            revokedOutputs,
            revokedFrames,
            revokedTransport)
                == activation::ComponentActivationAudioOperationV1::Quarantined
            && std::all_of(
                revokedOutput.begin(),
                revokedOutput.begin() + revokedFrames,
                [](float value) { return value == 0.0F; }),
        "revoke-before-read must fail closed without publishing component work");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && !evidence.active(),
        "revoked Audio must withdraw runtime evidence and quarantine the host");
    prepared.host->shutdown();
    require(
        waitForMaterializationCount(initialMaterializationCount)
            && waitForPeerExit(peer),
        "revoked Audio must leave no materialization, mapping owner, or helper process");
    return;
#endif

    constexpr std::size_t frames = TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES;
    constexpr std::size_t profileChannels = TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS;
    constexpr double sourceIncrement =
        48'000.0 / TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE;
    std::array<std::array<float, 512>, 2> input{};
    std::array<std::array<float, 512>, 2> output{};
    std::array<const float*, 2> inputs{};
    std::array<float*, 2> outputs{};
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        inputs[channel] = input[channel].data();
        outputs[channel] = output[channel].data();
    }
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1U;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "Audio callback one should publish without entering Runtime");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the first decoded Audio response");
    transport.sourceFrame = frames;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "Audio callback two should consume the first Runtime response");
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto expectedSourceFrame =
                static_cast<double>(frame) * sourceIncrement
                + static_cast<double>(channel * 4'096U);
            require(
                std::abs(
                    output[channel][frame]
                    - static_cast<float>(
                        expectedSourceFrame / 32'767.0)) < 1.0e-6F,
                "Runtime output should preserve channels and source-rate interpolation");
        }
    }

    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the sequential response before EOF setup");
    const auto partialEofStart = static_cast<std::int64_t>(
        8'044.0 / sourceIncrement);
    const auto eofWarmPosition = partialEofStart
        - static_cast<std::int64_t>(frames);
    transport.sourceFrame = eofWarmPosition;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "EOF setup discontinuity should preserve outer one-block delivery");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the bounded EOF setup underrun");
    transport.sourceFrame = partialEofStart;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "continuous partial-EOF request should remain active while the final source window refills");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the partial-EOF result");
    transport.sourceFrame = partialEofStart + frames;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "outer callback should consume the partial-EOF response");
    for (std::size_t channel = 0; channel < profileChannels; ++channel)
    {
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto expectedSourceFrame =
                static_cast<double>(partialEofStart) * sourceIncrement
                + static_cast<double>(frame) * sourceIncrement;
            if (expectedSourceFrame >= 8'192.0)
            {
                require(
                    output[channel][frame] == 0.0F,
                    "partial EOF must zero-fill the unavailable suffix");
                continue;
            }
            const auto interpolatedSourceValue =
                std::min(expectedSourceFrame, 8'191.0);
            const auto expected = static_cast<float>(
                (interpolatedSourceValue
                    + static_cast<double>(channel * 4'096U))
                / 32'767.0);
            if (std::abs(output[channel][frame] - expected) >= 1.0e-6F
                && output[channel][frame] != 0.0F)
            {
                std::cerr << "partial EOF mismatch channel=" << channel
                          << " frame=" << frame
                          << " source=" << expectedSourceFrame
                          << " expected=" << expected
                          << " actual=" << output[channel][frame] << '\n';
                require(
                    false,
                    "partial EOF must expose only exact prepared samples or deterministic underrun silence");
            }
        }
    }

    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the second sequential response");
    const auto jumpStart = static_cast<std::int64_t>(frames * 4U);
    const auto recoveryStart = jumpStart + static_cast<std::int64_t>(frames);
    transport.sourceFrame = jumpStart;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "outer transport discontinuity should remain one-block delayed");
    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the discontinuity callback response");
    transport.sourceFrame = recoveryStart;
    require(
        prepared.host->processAudio(
            {inputs.data(), profileChannels},
            {outputs.data(), profileChannels},
            frames,
            transport)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "post-discontinuity callback should remain bounded and active");
    require(
        std::all_of(
            output.begin(),
            output.begin() + profileChannels,
            [](const auto& channel) {
                return std::all_of(
                    channel.begin(),
                    channel.begin() + frames,
                    [](float value) { return value == 0.0F; });
            }),
        "an unprepared discontinuity should deterministically zero-fill once");

    require(
        waitForPublishedAudioResponse([&] {
            return prepared.host->publishedAudioResponseReadyForTesting();
        }),
        "Runtime should publish the recovered stream response");
    auto submittedSourceFrame =
        recoveryStart + static_cast<std::int64_t>(frames);
    auto expectedResponseSourceFrame = recoveryStart;
    bool recovered = false;
    constexpr std::size_t maximumRecoveryCallbacks =
        activation::maximumComponentActivationAudioTouchedSourceBlocksV1;
    for (std::size_t attempt = 0;
         attempt < maximumRecoveryCallbacks && !recovered;
         ++attempt)
    {
        transport.sourceFrame = submittedSourceFrame;
        require(
            prepared.host->processAudio(
                {inputs.data(), profileChannels},
                {outputs.data(), profileChannels},
                frames,
                transport)
                == activation::ComponentActivationAudioOperationV1::Rendered,
            "stream recovery callback should remain bounded and active");

        recovered = true;
        for (std::size_t channel = 0; channel < profileChannels; ++channel)
        {
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                const auto expectedSourceFrame =
                    (static_cast<double>(expectedResponseSourceFrame)
                        + static_cast<double>(frame))
                        * sourceIncrement
                    + static_cast<double>(channel * 4'096U);
                const auto expected = static_cast<float>(
                    expectedSourceFrame / 32'767.0);
                const auto exact = std::abs(
                    output[channel][frame] - expected) < 1.0e-6F;
                require(
                    exact || output[channel][frame] == 0.0F,
                    "recovery must expose only exact prepared samples or deterministic underrun silence");
                recovered = recovered && exact;
            }
        }
        if (recovered)
            break;
        require(
            waitForPublishedAudioResponse([&] {
                return prepared.host->publishedAudioResponseReadyForTesting();
            }),
            "Runtime should publish each bounded recovery response");
        const auto callbackPeriod = std::chrono::nanoseconds{
            static_cast<std::int64_t>(
                static_cast<std::uint64_t>(frames) * 1'000'000'000ULL
                / static_cast<std::uint64_t>(
                    TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE))};
        std::this_thread::sleep_for(callbackPeriod);
        expectedResponseSourceFrame = submittedSourceFrame;
        submittedSourceFrame += static_cast<std::int64_t>(frames);
    }
    require(
        recovered,
        "the independent stream worker should recover within the bounded callback exchange");

    require(
        waitForMaterializationCount(initialMaterializationCount),
        "active Audio must release its private executable materialization");
    prepared.host->shutdown();
    require(
        waitForMaterializationCount(initialMaterializationCount)
            && prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Closed
            && !evidence.active() && waitForPeerExit(peer),
        "active Audio teardown must stop workers and leave no helper residue");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 14 || \
    TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled
            && waitForTerminalState(
                *prepared.host,
                activation::ComponentActivationMacOSHostStateV1::
                    PreparedActivation),
        "active resource graph fixture should prepare one exact isolated instance");

    const auto document = transientComponentDocument(
        activationFixture.snapshot);
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto resourceSnapshot = activationFixture.inputs.resourceSnapshot;
    require(resourceSnapshot != nullptr,
            "active resource graph fixture should retain its exact snapshot");
    talas::runtime::ProjectRuntimeBridge ordinary(document, registry);
    require(
        ordinary.compileAndStage(
                    transientRenderSettings(resourceSnapshot))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "ordinary Open/Play must keep the resource capsule preserve-only");

    auto binding = std::move(*prepared.host)
                       .takePreparedIsolatedComponentBinding(
                           "component",
                           activationFixture.snapshot,
                           activationFixture.parameters);
    const auto* immutableContract = binding != nullptr
            && binding->contract().resources.has_value()
            && binding->contract().resources->members.size() == 1
        ? std::get_if<
            talas::runtime::IsolatedComponentImmutableResourceContract>(
            &binding->contract().resources->members.front())
        : nullptr;
    require(
        binding != nullptr && binding->active()
            && immutableContract != nullptr
            && immutableContract->parameterId == "table"
            && immutableContract->resourceId
                == resourceSnapshot->id
            && immutableContract->kind
                == resourceSnapshot->kind
            && immutableContract->generation
                == resourceSnapshot->generation
            && immutableContract->sourceContentHash
                == resourceSnapshot->contentHash
            && immutableContract->elementCount
                == (resourceSnapshot->kind
                            == talas::core::ResourceSnapshotKind::Binary
                        ? resourceSnapshot->binaryBytes.size()
                        : resourceSnapshot->tableSamples.size())
            && immutableContract->byteCount
                == (resourceSnapshot->kind
                            == talas::core::ResourceSnapshotKind::Binary
                        ? resourceSnapshot->binaryBytes.size()
                        : resourceSnapshot->tableSamples.size()
                            * sizeof(double)),
        "the active host should author one exact value-only resource graph contract");

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    const auto staged = bridge.compileAndStageIsolatedComponent(
        document,
        transientRenderSettings(resourceSnapshot),
        binding);
    require(
        staged.ok(),
        "the exact resource capsule should stage only with its host-authored binding");
    constexpr auto frames = static_cast<std::size_t>(
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES);
    std::array<float, 512> graphInput{};
    std::array<float, 512> graphOutput{};
    graphInput.fill(2.0F);
    const std::array<const float*, 1> graphInputs{graphInput.data()};
    const talas::runtime::RealtimeAudioInputBlock graphInputBlock{
        graphInputs.data(), graphInputs.size()};
    std::array<float*, 1> graphOutputs{graphOutput.data()};
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            frames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + frames,
                [](float value) { return value == 0.0F; }),
        "the first active resource graph callback should adopt and remain silent");
    require(
        waitForPublishedAudioResponse([&] {
            return binding->publishedAudioResponseReadyForTesting();
        }),
        "the staged helper should publish the exact immutable-resource response before callback two");
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            frames),
        "the second active resource graph callback should render");
#if TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 16
    constexpr std::array expected{0.0F, 254.0F, 256.0F, 508.0F, 510.0F};
#else
    constexpr std::array expected{0.5F, 1.0F, 1.5F};
#endif
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(
            graphOutput[frame] == expected[frame % expected.size()],
            "the active graph should read the exact retained helper resource mapping");
    }
    require(
        binding->active(),
        "the observed response must be consumed without quarantining the binding");
    binding.reset();
    const auto accepted = bridge.acceptedResourceIdentity(
        resourceSnapshot->id);
    require(
        accepted.has_value()
            && accepted->kind == resourceSnapshot->kind
            && accepted->generation == resourceSnapshot->generation
            && accepted->contentHash == resourceSnapshot->contentHash
            && registry.findNodeType(
                   activationFixture.snapshot.manifest.nodes.front().typeId)
                == nullptr
            && talas::project::serializeProjectDocument(document)
                == canonicalBefore,
        "active resource staging must record only exact history without canonical mutation");

    require(
        bridge.compileAndStageDocument(
                  directAudioDocument(), transientRenderSettings())
                .ok()
            && bridge.render(
                graphInputBlock,
                graphOutputs.data(),
                graphOutputs.size(),
                frames),
        "a built-in graph should replace the active resource graph at a callback boundary");
    require(
        bridge.collectRetired(),
        "off-callback collection should reclaim the resource host and mapping");
    require(
        !evidence.active()
            && waitForPeerExit(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "resource binding reclamation should stop its exact peer and leave no materialization file");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 10
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled
            && waitForTerminalState(
                *prepared.host,
                activation::ComponentActivationMacOSHostStateV1::
                    PreparedActivation),
        "transient graph fixture should prepare one exact isolated instance");

    const auto document = transientComponentDocument(
        activationFixture.snapshot);
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge ordinary(document, registry);
    require(
        ordinary.compileAndStage(transientRenderSettings()).outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "ordinary Open/Play must keep the exact fixture capsule preserve-only");

    auto binding = std::move(*prepared.host)
                       .takePreparedIsolatedComponentBinding(
                           "component",
                           activationFixture.snapshot,
                           activationFixture.parameters);
    require(
        binding != nullptr && binding->active()
            && binding->contract().contentDigest
                == activationFixture.candidate.contentDigest
            && binding->contract().signerIdentityDigest
                == activationFixture.candidate.signerIdentityDigest
            && binding->contract().requestedPermissions
                == activationFixture.candidate.requestedPermissions
            && binding->contract().policyVersion
                == activationFixture.candidate.policyVersion,
        "the prepared host should transfer one private live graph binding with its exact candidate fingerprint");
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    const auto staged = bridge.compileAndStageIsolatedComponent(
        document,
        transientRenderSettings(),
        binding);
    if (!staged.ok())
    {
        std::cerr << "transient graph stage failed: " << staged.message
                  << " outcome=" << static_cast<unsigned>(staged.outcome)
                  << '\n';
        for (const auto& diagnostic : staged.diagnostics)
        {
            std::cerr << "  node=" << diagnostic.nodeId
                      << " parameter=" << diagnostic.parameterId
                      << " message=" << diagnostic.message << '\n';
        }
    }
    require(
        staged.ok(),
        "the exact preserved fixture should stage only with its private binding");
    constexpr auto frames = static_cast<std::size_t>(
        TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES);
    std::array<float, 512> graphInput{};
    std::array<float, 512> graphOutput{};
    graphInput.fill(2.0F);
    const std::array<const float*, 1> graphInputs{graphInput.data()};
    const talas::runtime::RealtimeAudioInputBlock graphInputBlock{
        graphInputs.data(), graphInputs.size()};
    std::array<float*, 1> graphOutputs{graphOutput.data()};
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            frames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + frames,
                [](float value) { return value == 0.0F; }),
        "the first graph callback should adopt the candidate and remain silent");
    require(
        waitForPublishedAudioResponse([&] {
            return binding->publishedAudioResponseReadyForTesting();
        }),
        "the staged helper should publish the gain response before callback two");
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            frames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + frames,
                [](float value) { return value == 1.0F; }),
        "the second graph callback should render the original isolated gain bytes");
    require(
        binding->active(),
        "the observed gain response must be consumed without quarantine");
    binding.reset();
    require(
        activationFixture.owner.revokeOwnerAction(
            activationFixture.candidate,
            activationFixture.owner.generation())
            .ok(),
        "owner revocation should invalidate the exact active graph lease");
    graphOutput.fill(9.0F);
    require(
        bridge.render(
            graphInputBlock,
            graphOutputs.data(),
            graphOutputs.size(),
            frames)
            && std::all_of(
                graphOutput.begin(),
                graphOutput.begin() + frames,
                [](float value) { return value == 0.0F; }),
        "owner revocation must make the next graph callback silent");
    const auto revocationDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (evidence.active()
        && std::chrono::steady_clock::now() < revocationDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(
        !evidence.active() && waitForPeerExit(peer),
        "callback-observed owner revocation should terminate the exact helper generation");
    require(
        registry.findNodeType(
            activationFixture.snapshot.manifest.nodes.front().typeId)
                == nullptr
            && talas::project::serializeProjectDocument(document)
                == canonicalBefore,
        "private activation must not mutate the canonical registry or project bytes");

    require(
        bridge.compileAndStageDocument(
                  directAudioDocument(), transientRenderSettings())
                .ok()
            && bridge.render(
                graphInputBlock,
                graphOutputs.data(),
                graphOutputs.size(),
                frames),
        "a built-in graph should replace the isolated graph at a callback boundary");
    require(
        bridge.collectRetired(),
        "off-callback collection should reclaim the transferred host binding");
    require(
        !evidence.active()
            && waitForPeerExit(peer)
            && waitForMaterializationCount(initialMaterializationCount),
        "binding reclamation should revoke evidence, stop the peer, and leave no materialization file");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 7
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled,
        "one exact failure fixture should schedule one frozen-byte activation");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined),
        "every loader/ABI failure must quarantine its exact generation");
    constexpr bool deadlineFailure =
        TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 6
        || TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 10
        || TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 15
        || TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 18;
    require(
        prepared.host->diagnostic()
            == (deadlineFailure
                    ? activation::ComponentActivationMacOSHostDiagnosticV1::
                        DeadlineExpired
                    : activation::ComponentActivationMacOSHostDiagnosticV1::
                        ActivationRejected)
            && !evidence.active(),
        "loader failures must publish only their closed host diagnostic");
    require(
        waitForMaterializationCount(initialMaterializationCount),
        "failed activation must remove its private materialization path");
    require(
        waitForPeerExit(peer),
        "failed activation must not leave its exact helper process alive");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 8
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled
            && waitForTerminalState(
                *prepared.host,
                activation::ComponentActivationMacOSHostStateV1::
                    PreparedActivation),
        "teardown failure fixtures must first prepare one exact instance");
    prepared.host->shutdown();
    constexpr bool deadlineFailure =
        TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 21
        || TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 24;
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Quarantined
            && prepared.host->diagnostic()
                == (deadlineFailure
                        ? activation::ComponentActivationMacOSHostDiagnosticV1::
                            DeadlineExpired
                        : activation::ComponentActivationMacOSHostDiagnosticV1::
                            ActivationRejected)
            && !evidence.active(),
        "destroy/unload failure must quarantine with one closed host diagnostic");
    require(
        waitForMaterializationCount(initialMaterializationCount),
        "teardown failure must leave no private materialization path");
    require(
        waitForPeerExit(peer),
        "teardown failure must not leave its exact helper process alive");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 9
    const auto initialMaterializationCount = activationMaterializationCount();
    auto lease = activationFixture.lease(84);
    require(
        lease.has_value()
            && prepared.host->activate(
                std::move(*lease),
                activationFixture.snapshot,
                activationFixture.parameters)
                == activation::ComponentActivationMacOSHostOperationV1::Scheduled
            && waitForTerminalState(
                *prepared.host,
                activation::ComponentActivationMacOSHostStateV1::
                    PreparedActivation),
        "runtime callback failure fixture must pass load, prepare, and canary");

    std::array<float, 256> audioInput{};
    std::array<float, 256> audioOutput{};
    std::fill(audioInput.begin(), audioInput.end(), 1.0F);
    const std::array<const float*, 1> audioInputs{audioInput.data()};
    const std::array<float*, 1> audioOutputs{audioOutput.data()};
    activation::ComponentActivationAudioTransportV1 transport;
    transport.generation = 1;
    constexpr auto callbackPeriod = std::chrono::nanoseconds{
        (256LL * 1'000'000'000LL) / 48'000LL};
    constexpr auto callbackBudget = callbackPeriod / 2;
    const auto firstStart = std::chrono::steady_clock::now();
    const auto firstOperation = prepared.host->processAudio(
        audioInputs, audioOutputs, 256, transport);
    const auto firstDuration =
        std::chrono::steady_clock::now() - firstStart;
    require(
        firstOperation
                == activation::ComponentActivationAudioOperationV1::
                    FirstBlockSilence
            && firstDuration < callbackBudget
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "runtime failure must not block or dirty the publishing callback");

    std::this_thread::sleep_for(callbackPeriod);
    std::fill(audioOutput.begin(), audioOutput.end(), 9.0F);
    transport.sourceFrame = 256;
    const auto secondStart = std::chrono::steady_clock::now();
    const auto secondOperation = prepared.host->processAudio(
        audioInputs, audioOutputs, 256, transport);
    const auto secondDuration =
        std::chrono::steady_clock::now() - secondStart;
    require(
        secondOperation
                != activation::ComponentActivationAudioOperationV1::Rendered
            && secondDuration < callbackBudget
            && std::all_of(
                audioOutput.begin(), audioOutput.end(), [](float value) {
                    return value == 0.0F;
                }),
        "runtime failure must return bounded silence without resurrection");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined),
        "runtime callback failure must quarantine its exact generation");
    constexpr bool deadlineFailure =
        TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 28
        || TALAS_COMPONENT_ACTIVATION_FAILURE_MODE == 32;
    require(
        prepared.host->diagnostic()
                == (deadlineFailure
                        ? activation::ComponentActivationMacOSHostDiagnosticV1::
                            DeadlineExpired
                        : activation::ComponentActivationMacOSHostDiagnosticV1::
                            ActivationRejected)
            && !evidence.active(),
        "runtime failure must publish one closed host diagnostic and revoke evidence");
    require(
        waitForMaterializationCount(initialMaterializationCount),
        "runtime failure must leave no private materialization path");
    require(
        waitForPeerExit(peer),
        "runtime failure must leave no exact helper process alive");
#else
    prepared.host->shutdown();
    require(
        prepared.host->state()
                == activation::ComponentActivationMacOSHostStateV1::Quarantined
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    ProcessTerminationFailed
            && !evidence.active(),
        "failed exact termination or identity validation must never publish Closed");
    const auto helperExitedOnItsOwn = waitForPeerExit(peer);
    if (!helperExitedOnItsOwn)
        static_cast<void>(kill(peer, SIGKILL));
    require(
        helperExitedOnItsOwn,
        "a helper the host could not terminate must exit once its only peer is gone");
#endif
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 2
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    ProtocolRejected
            && !prepared.host->runtimeEvidence().active(),
        "a malformed sealed acknowledgement must quarantine without evidence");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 3
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    DeadlineExpired
            && !prepared.host->runtimeEvidence().active(),
        "a hanging sealed peer must hit the parent watchdog");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 4
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    ProtocolRejected
            && !prepared.host->runtimeEvidence().active(),
        "a reply for another sealed session must quarantine");
#elif TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO == 5
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    LaunchRejected
            && !prepared.host->runtimeEvidence().active(),
        "a missing exact XPC peer must fail closed without evidence");
#else
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Ready),
        "heartbeat-hang fixture should first publish sealed evidence");
    auto evidence = prepared.host->runtimeEvidence();
    require(evidence.active(),
        "heartbeat-hang fixture should start with live runtime evidence");
    require(
        waitForTerminalState(
            *prepared.host,
            activation::ComponentActivationMacOSHostStateV1::Quarantined)
            && prepared.host->diagnostic()
                == activation::ComponentActivationMacOSHostDiagnosticV1::
                    DeadlineExpired
            && !evidence.active(),
        "a missed live heartbeat must revoke already-issued runtime evidence");
#endif
}
}

template<class Host>
void requireAuthenticatedCheckpointControl()
{
    if constexpr (!requires(Host& host) {
        host.queryOfflineCheckpoint(); host.beginOfflineCheckpoint();
        host.restoreOfflineCheckpoint(); host.commitOfflineCheckpoint();
    }) require(false, "actual macOS host lacks authenticated helper checkpoint control");
}

#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 11 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 13
void testQueuedActivationLifecycle()
{
    using Host = activation::ComponentActivationMacOSHost;
    using State = activation::ComponentActivationMacOSHostStateV1;
    using Diagnostic = activation::ComponentActivationMacOSHostDiagnosticV1;
    using Operation = activation::ComponentActivationMacOSHostOperationV1;
    constexpr unsigned mode = TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION - 11;
    {
        const auto filesBefore = activationMaterializationCount();
        auto prepared = Host::prepare(activation::ComponentActivationMacOSEndpointV1::Production,
            activation::ComponentActivationMacOSProfileV1::BoundedAudioStream);
        require(prepared.ok() && prepared.host->start() == Operation::Scheduled
            && waitForTerminalState(*prepared.host, State::Ready), "queued activation fixture becomes Ready");
        auto& host = *prepared.host;
        auto evidence = host.runtimeEvidence();
        const auto peer = host.peerProcessIdentifier();
        ActivationAuthorityFixture fixture{host};
        host.overlapNextActivationHeartbeatForTesting(true);
        auto lease = fixture.lease(84);
        require(lease.has_value() && host.activate(std::move(*lease), fixture.snapshot, fixture.parameters)
            == Operation::Scheduled, "activation queues behind a real sealed heartbeat");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
        while (!host.activationHeartbeatHeldForTesting() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(host.activationHeartbeatHeldForTesting() && host.state() == State::Ready
            && host.activationMessageCountForTesting() == 0, "queued activation owns its message without sending component bytes");
        auto duplicate = fixture.lease(85);
        require(duplicate.has_value() && host.activate(std::move(*duplicate), fixture.snapshot, fixture.parameters)
            == Operation::InvalidState, "another activation cannot replace queued ownership");
        if (mode == 1) host.shutdown();
        else if (mode == 2)
        {
            require(fixture.owner.replaceOwnerAction(fixture.candidate,
                bundle::ComponentOwnerAuthorizationAction::Deny, fixture.owner.generation()).ok(),
                "owner revokes a queued activation before its heartbeat completes");
            host.releaseActivationHeartbeatForTesting();
        }
        require(waitForTerminalState(host, State::Quarantined) && !evidence.active()
            && host.diagnostic() == (mode == 2 ? Diagnostic::Revoked : Diagnostic::DeadlineExpired),
            "queued timeout, shutdown wait and revoked authority fail closed with exact diagnostics");
        host.releaseActivationHeartbeatForTesting();
        require(host.activationMessageCountForTesting() == 0 && waitForPeerExit(peer)
            && waitForMaterializationCount(filesBefore), "terminal queued activation sends no bytes and retires message, image and peer");
    }
}
#endif

int main()
{
#if TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION >= 11 && TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION <= 13
    testQueuedActivationLifecycle();
#else
    testSealedRuntimeLifecycle();
#endif
    requireAuthenticatedCheckpointControl<activation::ComponentActivationMacOSHost>();
    return 0;
}
