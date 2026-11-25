#include "runtime/component_activation/macos/ComponentActivationLoader.h"
#include "runtime/component_bundle/ComponentBundleInspection.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <new>
#include <limits>
#include <mach-o/dyld.h>

namespace
{
thread_local std::size_t ordinaryAllocations = 0;
thread_local std::size_t allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
thread_local bool allocationFaultTriggered = false;
}
void* operator new(std::size_t size)
{
    ++ordinaryAllocations;
    if (allocationFaultCountdown != std::numeric_limits<std::size_t>::max()
        && allocationFaultCountdown-- == 0)
    {
        allocationFaultTriggered = true;
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
namespace activation = talas::runtime::component_activation;
namespace bundle = talas::runtime::component_bundle;
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

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

struct Fixture
{
    activation::ComponentActivationSealedSessionV1 session;
    activation::ComponentActivationAuthorizationV1 authorization;
    std::array<activation::ComponentActivationStaticParameterV1, 1> parameters;
    bundle::ComponentBundleSnapshot snapshot;
    std::vector<std::uint8_t> entryBytes;

    explicit Fixture(
        const char* fixturePath = TALAS_CHECKPOINT_FIXTURE_0,
        int mode = 0)
    {
        std::ifstream stream(fixturePath, std::ios::binary);
        require(stream.good(), "resource loader fixture should open");
        entryBytes.assign(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
        require(!entryBytes.empty(), "resource loader fixture should have bytes");
        const auto entryDigest =
            activation::calculateComponentActivationByteDigestV1(entryBytes);

        std::ostringstream json;
        json << R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.talas.activation.fixture",
  "semanticVersion":"1.0.0",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":1,"apiMaximum":1},
  "roles":["realtime-processor"],
  "permissions":["realtime-audio"],
  "entry":{"path":"Payload/resource.bundle","bytes":)json"
             << entryBytes.size() << R"json(,"sha256":")json"
             << digestHex(entryDigest) << R"json("},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.fixture.gain",
    "stateSchemaVersion":1,
    "inputs":[{"id":"in","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "outputs":[{"id":"out","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "parameters":[{"id":"mode","kind":"expression","domain":"scalar","choices":[]}]
  }],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Talas tests","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
        const auto parsed = bundle::parseComponentBundleManifest(json.str());
        require(parsed.ok(), "resource loader manifest should parse");
        const auto canonical =
            bundle::writeCanonicalComponentBundleManifest(*parsed.manifest);
        require(canonical.ok(), "resource loader manifest should canonicalize");
        snapshot.manifest = *parsed.manifest;
        snapshot.canonicalManifest = canonical.canonicalJson;
        snapshot.payloads.push_back({
            bundle::ComponentBundleFilePurpose::Resource,
            snapshot.manifest.entry.path,
            entryDigest,
            entryBytes,
            true,
        });
        snapshot.contentDigest =
            bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
        require(
            bundle::validateComponentBundleSnapshotIdentity(snapshot),
            "resource loader snapshot should be exact");

        session.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::SealedSession,
            sizeof(session));
        session.profileVersion = activation::componentActivationProfileVersionV1;
        session.supportedPermissions = componentPermission(ComponentExecutionPermission::RealtimeAudio);
        session.safeguards = allComponentIsolationSafeguardsV1;
        session.runtimeSessionGeneration = 17;
        session.sharedStorageIdentity = 23;
        session.sharedStorageBytes = 65'536;
        session.runtimeSessionNonce.fill(0x33);

        auto& parameter = parameters.front();
        parameter.kind = static_cast<std::uint32_t>(activation::ComponentActivationStaticParameterKindV1::Scalar);
        constexpr std::string_view parameterId = "mode";
        parameter.idBytes = parameterId.size();
        std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
        parameter.scalarValue = mode;

        authorization.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::Authorization,
            sizeof(authorization));
        const auto& manifest = snapshot.manifest;
        authorization.bundleIdBytes = manifest.bundleId.size();
        authorization.semanticVersionBytes = manifest.semanticVersion.size();
        constexpr std::string_view descriptorId =
            "org.talas.activation.fixture.gain";
        authorization.descriptorIdBytes = descriptorId.size();
        std::copy(
            manifest.bundleId.begin(), manifest.bundleId.end(),
            authorization.bundleId.begin());
        std::copy(
            manifest.semanticVersion.begin(), manifest.semanticVersion.end(),
            authorization.semanticVersion.begin());
        std::copy(
            descriptorId.begin(), descriptorId.end(),
            authorization.descriptorId.begin());
        authorization.contentDigest = snapshot.contentDigest;
        authorization.signerIdentityDigest.fill(0x42);
        authorization.entryDigest = entryDigest;
        authorization.requestedPermissions = manifest.requestedPermissions;
        authorization.declaredRoles = manifest.declaredRoles;
        authorization.policyVersion = currentComponentTrustPolicyVersion;
        authorization.sampleRate = 48'000;
        authorization.blockFrames = 256;
        authorization.inputChannelCount = 1;
        authorization.outputChannelCount = 1;
        authorization.staticParameterCount = parameters.size();
        authorization.staticParameterDigest =
            activation::calculateComponentActivationStaticParameterDigestV1(
                parameters);
        authorization.entryByteCount = entryBytes.size();
        authorization.artifactGeneration = 29;
        authorization.activationGeneration = 31;
        authorization.runtimeSessionGeneration = session.runtimeSessionGeneration;
        authorization.sharedStorageIdentity = session.sharedStorageIdentity;
        authorization.validationNonce.fill(0x5c);
        authorization.runtimeSessionNonce = session.runtimeSessionNonce;
        authorization.authorizationNonce.fill(0xa5);

    }
};

struct MaterializationFile
{
    int descriptor = -1;
    std::filesystem::path path;

    MaterializationFile()
    {
        char fileTemplate[] =
            "/private/tmp/talas-resource-loader-materialization.XXXXXX";
        descriptor = mkstemp(fileTemplate);
        require(descriptor >= 0, "resource loader materialization should allocate");
        path = fileTemplate;
        require(
            fchmod(descriptor, 0500) == 0,
            "resource loader materialization should have exact mode");
    }

    ~MaterializationFile()
    {
        if (descriptor >= 0)
            static_cast<void>(close(descriptor));
    }

    int release() noexcept
    {
        const auto result = descriptor;
        descriptor = -1;
        return result;
    }
};

activation::ComponentActivationLoaderResultV1 loadFixture(Fixture& fixture)
{
    MaterializationFile materialization;
    return activation::loadComponentActivationV1(fixture.authorization, fixture.session,
        {reinterpret_cast<const std::uint8_t*>(fixture.snapshot.canonicalManifest.data()), fixture.snapshot.canonicalManifest.size()},
        fixture.entryBytes, fixture.parameters, materialization.release());
}

double inputAt(std::size_t frame)
{
    return .125 * static_cast<double>(static_cast<int>((frame * 3) % 11) - 5);
}
std::vector<double> oracle(std::size_t frames)
{
    std::vector<double> result(frames);
    double history = 0;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        history = history * .5 + inputAt(frame);
        result[frame] = history;
    }
    return result;
}
std::vector<double> render(activation::ComponentActivationLoadedInstanceV1& instance,
    std::size_t start, std::size_t frames)
{
    std::vector<double> input(frames), output(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) input[frame] = inputAt(start + frame);
    TalasKernelAbiSignalV1 signal{};
    signal.structSize = sizeof(signal); signal.kind = TALAS_KERNEL_ABI_SIGNAL_PER_FRAME;
    signal.samples = input.data(); signal.sampleCount = frames;
    TalasKernelAbiOutputV1 outputRecord{};
    outputRecord.structSize = sizeof(outputRecord); outputRecord.samples = output.data(); outputRecord.sampleCount = frames;
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process); process.frameCount = frames;
    process.inputs = &signal; process.inputCount = 1;
    process.outputs = &outputRecord; process.outputCount = 1;
    process.transport.structSize = sizeof(process.transport);
    process.transport.sampleRate = 48000; process.transport.absoluteFrame = start; process.transport.playheadFrame = start;
    require(instance.process(process) == TALAS_KERNEL_ABI_STATUS_OK, "stateful loaded kernel should render");
    return output;
}
void requireContinuation(const std::vector<double>& actual, const std::vector<double>& expected, std::size_t start)
{
    require(start + actual.size() <= expected.size(), "oracle range must be valid");
    require(std::equal(actual.begin(), actual.end(), expected.begin() + static_cast<std::ptrdiff_t>(start)),
        "loaded checkpoint continuation must match independent recurrence exactly");
}

void testLoadedKernelRepeatedRollbackAndCommit()
{
    for (const std::size_t prefix : {0U, 7U})
    {
        Fixture fixture;
        auto loaded = loadFixture(fixture);
        require(loaded.ok() && loaded.instance->supportsKernelOfflineCheckpoint(), "authenticated image should expose its validated extension");
        auto& instance = *loaded.instance;
        const auto expected = oracle(64);
        if (prefix != 0) requireContinuation(render(instance, 0, prefix), expected, 0);
        for (std::size_t attempt = 0; attempt < 128; ++attempt)
        {
            require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                "repeated captures must not exhaust the preparation lifetime allocation budget");
            require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                "nested capture must retain the original baseline");
            requireContinuation(render(instance, prefix, 17), expected, prefix);
            TalasKernelAbiResetV1 reset{};
            reset.structSize = sizeof(reset); reset.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT;
            reset.transport.structSize = sizeof(reset.transport); reset.transport.sampleRate = 48000;
            require(instance.reset(reset) == TALAS_KERNEL_ABI_STATUS_OK, "provisional reset should succeed");
            static_cast<void>(render(instance, 31, 13));
            const auto before = ordinaryAllocations;
            const auto restored = instance.restoreKernelOfflineCheckpoint();
            require(restored == TALAS_KERNEL_ABI_STATUS_OK && ordinaryAllocations == before,
                "restore and payload cleanup must allocate no ordinary memory");
            require(instance.active(), "successful restore must retain the instance");
        }
        instance.failNextCheckpointAllocationForTesting();
        require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR && instance.active(),
            "physical checkpoint allocation failure must preserve retryable processing state");
        require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "capture should recover after physical allocation failure");
        requireContinuation(render(instance, prefix, 17), expected, prefix);
        const auto before = ordinaryAllocations;
        const auto committed = instance.commitKernelOfflineCheckpoint();
        require(committed == TALAS_KERNEL_ABI_STATUS_OK && ordinaryAllocations == before,
            "commit must free checkpoint memory without allocating or restoring history");
        require(instance.restoreKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
            "committed baseline cannot restore twice");
        requireContinuation(render(instance, prefix + 17, 17), expected, prefix + 17);
        require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "shutdown fixture should hold a baseline");
        require(instance.shutdown() && instance.shutdown(), "shutdown must release snapshot before instance and image, exactly once");
    }
}

void testLoadedCheckpointDiscoveryAndAuthorization()
{
    Fixture legacy{TALAS_CHECKPOINT_FIXTURE_1};
    auto loaded = loadFixture(legacy);
    require(loaded.ok() && !loaded.instance->supportsKernelOfflineCheckpoint()
        && loaded.instance->beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE,
        "missing optional entry must preserve legacy admission without inventing support");
    requireContinuation(render(*loaded.instance, 0, 7), oracle(7), 0);
    require(loaded.instance->shutdown(), "legacy image should close cleanly");
    for (const auto path : {TALAS_CHECKPOINT_FIXTURE_2, TALAS_CHECKPOINT_FIXTURE_3})
    {
        const auto images = _dyld_image_count();
        Fixture invalid{path};
        const auto rejected = loadFixture(invalid);
        require(!rejected.ok() && rejected.diagnostic == activation::ComponentActivationLoaderDiagnosticV1::AbiRejected,
            "present malformed or throwing checkpoint entry must reject before factory callbacks");
        require(_dyld_image_count() == images, "extension rejection must release its materialized image");
        invalid.authorization.activationGeneration = 0;
        require(loadFixture(invalid).diagnostic == activation::ComponentActivationLoaderDiagnosticV1::ProtocolRejected,
            "invalid authorization must reject before optional entry execution");
    }
}

void testPreparedImagePublicationAllocationFailureCleansLifetime()
{
    Fixture fixture;
    const auto before = ordinaryAllocations;
    auto baseline = loadFixture(fixture);
    const auto count = ordinaryAllocations - before;
    require(baseline.ok() && count != 0, "prepared publication fault measurement should load");
    require(baseline.instance->shutdown(), "measurement image should close before injected failure");
    const auto images = _dyld_image_count();
    allocationFaultTriggered = false;
    allocationFaultCountdown = count - 1;
    const auto rejected = loadFixture(fixture);
    allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
    require(allocationFaultTriggered && !rejected.ok()
        && rejected.diagnostic == activation::ComponentActivationLoaderDiagnosticV1::AllocationFailed,
        "final loaded-instance publication allocation should fail inside the loader");
    require(_dyld_image_count() == images,
        "failed publication after prepare must destroy instance and unload its image");
}

void testLoadedCaptureFailureCleanupAndRetainedFatalState()
{
    for (int mode = 1; mode <= 10; ++mode)
    {
        Fixture fixture{TALAS_CHECKPOINT_FIXTURE_0, mode};
        auto loaded = loadFixture(fixture);
        require(loaded.ok(), "failure-mode fixture should prepare");
        auto& instance = *loaded.instance;
        const auto expected = oracle(32);
        requireContinuation(render(instance, 0, 7), expected, 0);
        const auto captured = instance.beginKernelOfflineCheckpoint();
        if (mode <= 2)
        {
            require(captured == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR && instance.active(),
                "cleanly rejected capture must preserve retry, including a partially returned payload");
            require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "rejected capture should release every domain allocation");
            requireContinuation(render(instance, 7, 17), expected, 7);
            require(instance.restoreKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "capture retry should restore");
            requireContinuation(render(instance, 7, 17), expected, 7);
            require(instance.shutdown(), "recovered image should have balanced lifetime accounting");
            continue;
        }
        if (mode == 3 || mode == 7)
            require(captured != TALAS_KERNEL_ABI_STATUS_OK && !instance.active(),
                "throwing capture or exceeded checkpoint budget must quarantine and clean payloads");
        else
        {
            require(captured == TALAS_KERNEL_ABI_STATUS_OK, "fatal restore boundary needs a captured baseline");
            if (mode == 10)
            {
                TalasKernelAbiResetV1 reset{};
                reset.structSize = sizeof(reset); reset.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT;
                reset.transport.structSize = sizeof(reset.transport); reset.transport.sampleRate = 48000;
                require(instance.reset(reset) == TALAS_KERNEL_ABI_STATUS_FATAL, "fixture should enter terminal processing failure");
            }
            require(instance.restoreKernelOfflineCheckpoint() != TALAS_KERNEL_ABI_STATUS_OK && !instance.active(),
                "failed restore, wrong-domain free, leak, exception or prior fatal state must stay quarantined");
        }
        require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
            "checkpoint recovery must never revive an unavailable instance");
        const bool clean = instance.shutdown();
        if (mode == 5 || mode == 6 || mode == 7 || mode == 8)
            require(!clean, "allocator contract violations must remain visible through shutdown");
    }
}
}
void testCoordinatedCheckpointWithoutResources()
{
    Fixture fixture;
    auto loaded = loadFixture(fixture);
    require(loaded.ok() && loaded.instance->supportsOfflineCheckpoint(), "kernel-only module can coordinate an empty resource set");
    const auto expected = oracle(32);
    require(loaded.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "empty resource capture should succeed");
    static_cast<void>(render(*loaded.instance, 0, 7));
    require(loaded.instance->restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "empty resource restore should succeed");
    requireContinuation(render(*loaded.instance, 0, 7), expected, 0);
    require(loaded.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "empty resource commit should capture");
    requireContinuation(render(*loaded.instance, 7, 7), expected, 7);
    require(loaded.instance->commitOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "empty resource commit should succeed");
    requireContinuation(render(*loaded.instance, 14, 7), expected, 14);
    require(loaded.instance->shutdown(), "empty resource coordinated instance should close cleanly");
}

int main()
{
    testLoadedKernelRepeatedRollbackAndCommit();
    testLoadedCheckpointDiscoveryAndAuthorization();
    testPreparedImagePublicationAllocationFailureCleansLifetime();
    testLoadedCaptureFailureCleanupAndRetainedFatalState();
    testCoordinatedCheckpointWithoutResources();
}
