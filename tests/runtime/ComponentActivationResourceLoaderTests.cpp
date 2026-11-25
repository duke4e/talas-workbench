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
    activation::ComponentActivationImmutableResourceGrantV1 grant;
    std::vector<std::uint8_t> resourceBytes;
    bundle::ComponentBundleSnapshot snapshot;
    std::vector<std::uint8_t> entryBytes;

    explicit Fixture(
        const char* fixturePath =
            TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE_PATH,
        bool binaryResource = false)
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
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":2,"apiMaximum":2},
  "roles":["immutable-resource-consumer","realtime-processor"],
  "permissions":["immutable-resource-read","realtime-audio"],
  "entry":{"path":"Payload/resource.bundle","bytes":)json"
             << entryBytes.size() << R"json(,"sha256":")json"
             << digestHex(entryDigest) << R"json("},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.fixture.gain",
    "stateSchemaVersion":1,
    "inputs":[{"id":"in","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "outputs":[{"id":"out","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "parameters":[{"id":"table","kind":"resource","domain":"scalar","choices":[]}]
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
        session.supportedPermissions = componentPermission(
                ComponentExecutionPermission::RealtimeAudio)
            | componentPermission(
                ComponentExecutionPermission::ImmutableResourceRead);
        session.safeguards = allComponentIsolationSafeguardsV1;
        session.runtimeSessionGeneration = 17;
        session.sharedStorageIdentity = 23;
        session.sharedStorageBytes = 65'536;
        session.runtimeSessionNonce.fill(0x33);

        auto& parameter = parameters.front();
        parameter.kind = static_cast<std::uint32_t>(
            activation::ComponentActivationStaticParameterKindV1::Resource);
        constexpr std::string_view parameterId = "table";
        constexpr std::string_view resourceId = "Tables/Curve-A";
        parameter.idBytes = parameterId.size();
        parameter.textBytes = resourceId.size();
        std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
        std::copy(resourceId.begin(), resourceId.end(), parameter.text.begin());

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

        constexpr std::array values{0.25, 0.5, 0.75};
        if (binaryResource)
        {
            resourceBytes = {0x00, 0x7f, 0x80, 0xfe, 0xff};
        }
        else
        {
            resourceBytes.resize(values.size() * sizeof(double));
            require(
                activation::encodeComponentActivationImmutableTableV1(
                    values, resourceBytes)
                    == activation::ComponentActivationProtocolDiagnosticV1::
                        Valid,
                "resource loader table should encode");
        }
        grant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::ImmutableResourceGrant,
            sizeof(grant));
        grant.contentDigest = authorization.contentDigest;
        grant.mediatedPayloadDigest =
            activation::calculateComponentActivationByteDigestV1(resourceBytes);
        if (binaryResource)
            grant.sourceContentDigest = grant.mediatedPayloadDigest;
        else
            grant.sourceContentDigest.fill(0x61);
        grant.validationNonce = authorization.validationNonce;
        grant.runtimeSessionNonce = authorization.runtimeSessionNonce;
        grant.authorizationNonce = authorization.authorizationNonce;
        grant.artifactGeneration = authorization.artifactGeneration;
        grant.activationGeneration = authorization.activationGeneration;
        grant.runtimeSessionGeneration = authorization.runtimeSessionGeneration;
        grant.sharedStorageIdentity = authorization.sharedStorageIdentity;
        grant.resourceGeneration = 7;
        grant.elementCount = binaryResource
            ? resourceBytes.size()
            : values.size();
        grant.byteCount = resourceBytes.size();
        grant.parameterIdBytes = parameterId.size();
        grant.resourceIdBytes = resourceId.size();
        grant.resourceKind = static_cast<std::uint32_t>(
            binaryResource
                ? activation::ComponentActivationImmutableResourceKindV1::
                    Binary
                : activation::ComponentActivationImmutableResourceKindV1::
                    Table);
        grant.elementFormat = static_cast<std::uint32_t>(
            binaryResource
                ? activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        RawBytes
                : activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
        std::copy(
            parameterId.begin(), parameterId.end(), grant.parameterId.begin());
        std::copy(resourceId.begin(), resourceId.end(), grant.resourceId.begin());
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

void testExactResourceMappingResolvesInPrepareAndReadsInProcess()
{
    Fixture fixture;
    MaterializationFile materialization;
    auto loaded = activation::loadComponentActivationV1(
        fixture.authorization,
        fixture.session,
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(
                fixture.snapshot.canonicalManifest.data()),
            fixture.snapshot.canonicalManifest.size()},
        fixture.entryBytes,
        fixture.parameters,
        materialization.release(),
        &fixture.grant,
        fixture.resourceBytes);
    if (!loaded.ok())
    {
        std::cerr << "resource loader diagnostic="
                  << static_cast<unsigned>(loaded.diagnostic) << '\n';
    }
    require(loaded.ok(), "exact resource fixture should load and prepare");

    constexpr std::size_t frames = 6;
    std::array<double, frames> output{};
    TalasKernelAbiSignalV1 input{};
    input.structSize = sizeof(input);
    input.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    input.constantValue = 1.0;
    TalasKernelAbiOutputV1 outputRecord{};
    outputRecord.structSize = sizeof(outputRecord);
    outputRecord.samples = output.data();
    outputRecord.sampleCount = output.size();
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process);
    process.frameCount = frames;
    process.inputs = &input;
    process.inputCount = 1;
    process.outputs = &outputRecord;
    process.outputCount = 1;
    require(
        loaded.instance->process(process) == TALAS_KERNEL_ABI_STATUS_OK,
        "resource fixture process should use its retained table pointer");
    constexpr std::array expected{0.25, 0.5, 0.75, 0.25, 0.5, 0.75};
    require(
        output == expected,
        "resource fixture output should prove direct immutable table reads");
    require(
        loaded.instance->shutdown(),
        "resource fixture should release mapping after destroy and unload");
}

void testExactBinaryMappingResolvesInPrepareAndReadsInProcess()
{
    Fixture fixture{
        TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE_PATH,
        true};
    MaterializationFile materialization;
    auto loaded = activation::loadComponentActivationV1(
        fixture.authorization,
        fixture.session,
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(
                fixture.snapshot.canonicalManifest.data()),
            fixture.snapshot.canonicalManifest.size()},
        fixture.entryBytes,
        fixture.parameters,
        materialization.release(),
        &fixture.grant,
        fixture.resourceBytes);
    require(loaded.ok(), "exact Binary resource fixture should load and prepare");

    constexpr std::size_t frames = 7;
    std::array<double, frames> output{};
    TalasKernelAbiSignalV1 input{};
    input.structSize = sizeof(input);
    input.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    input.constantValue = 1.0;
    TalasKernelAbiOutputV1 outputRecord{};
    outputRecord.structSize = sizeof(outputRecord);
    outputRecord.samples = output.data();
    outputRecord.sampleCount = output.size();
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process);
    process.frameCount = frames;
    process.inputs = &input;
    process.inputCount = 1;
    process.outputs = &outputRecord;
    process.outputCount = 1;
    require(
        loaded.instance->process(process) == TALAS_KERNEL_ABI_STATUS_OK,
        "Binary fixture process should use its retained raw-byte pointer");
    constexpr std::array expected{0.0, 127.0, 128.0, 254.0, 255.0, 0.0, 127.0};
    require(
        output == expected,
        "Binary fixture output must prove direct zero and high-bit octet reads");
    require(
        loaded.instance->shutdown(),
        "Binary fixture should retain its mapping through destroy and unload");
}

activation::ComponentActivationLoaderResultV1 loadFixture(
    Fixture& fixture,
    const activation::ComponentActivationImmutableResourceGrantV1* grant,
    std::span<const std::uint8_t> bytes)
{
    MaterializationFile materialization;
    return activation::loadComponentActivationV1(
        fixture.authorization,
        fixture.session,
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(
                fixture.snapshot.canonicalManifest.data()),
            fixture.snapshot.canonicalManifest.size()},
        fixture.entryBytes,
        fixture.parameters,
        materialization.release(),
        grant,
        bytes);
}

void testResourceLoaderRejectsDetachedGrantAndPayloadBeforeLoad()
{
    Fixture missing;
    require(
        loadFixture(missing, nullptr, {}).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                ResourceRejected,
        "resource loader must reject a missing opaque grant and payload");

    Fixture changed;
    auto changedBytes = changed.resourceBytes;
    changedBytes.front() ^= 0x01U;
    require(
        loadFixture(changed, &changed.grant, changedBytes).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                ResourceRejected,
        "resource loader must reject same-sized mediated-byte mutation");

    Fixture stale;
    auto invalidGeneration = stale.grant;
    invalidGeneration.resourceGeneration = 0;
    require(
        loadFixture(stale, &invalidGeneration, stale.resourceBytes).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                ResourceRejected,
        "resource loader must reject an invalid resource generation");
}

void testBinaryLoaderRejectsEveryDetachedTupleBeforeLoad()
{
    Fixture fixture{
        TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE_PATH,
        true};
    auto changedBytes = fixture.resourceBytes;
    changedBytes[2] ^= 0xffU;
    require(
        loadFixture(fixture, &fixture.grant, changedBytes).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                ResourceRejected,
        "Binary loader must reject same-sized raw-byte mutation");

    const auto rejectsGrant = [&](auto mutate, const char* message) {
        auto changed = fixture.grant;
        mutate(changed);
        require(
            loadFixture(fixture, &changed, fixture.resourceBytes).diagnostic
                == activation::ComponentActivationLoaderDiagnosticV1::
                    ResourceRejected,
            message);
    };
    rejectsGrant([](auto& value) { ++value.elementCount; },
        "Binary loader must reject element-count drift");
    rejectsGrant(
        [](auto& value) {
            value.resourceKind = static_cast<std::uint32_t>(
                activation::ComponentActivationImmutableResourceKindV1::
                    Table);
        },
        "Binary loader must reject a Table kind claim");
    rejectsGrant(
        [](auto& value) {
            value.elementFormat = static_cast<std::uint32_t>(
                activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
        },
        "Binary loader must reject a Table format claim");
    rejectsGrant(
        [](auto& value) { value.sourceContentDigest.front() ^= 0xffU; },
        "Binary loader must reject source identity drift");
    rejectsGrant(
        [](auto& value) { value.mediatedPayloadDigest.front() ^= 0xffU; },
        "Binary loader must reject mediated identity drift");
    require(
        loadFixture(fixture, &fixture.grant, {}).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                ResourceRejected,
        "Binary loader must reject an empty transferred payload");
}

void testResolverFailsClosedAfterPreparationWithoutCallbackLocking()
{
    Fixture fixture{
        TALAS_COMPONENT_ACTIVATION_RESOURCE_LATE_RESOLVER_FIXTURE_PATH};
    auto loaded = loadFixture(
        fixture, &fixture.grant, fixture.resourceBytes);
    require(
        loaded.ok(),
        "late-resolver adversarial fixture should first prepare exactly once");

    double output = 0.0;
    TalasKernelAbiSignalV1 input{};
    input.structSize = sizeof(input);
    input.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    input.constantValue = 1.0;
    TalasKernelAbiOutputV1 outputRecord{};
    outputRecord.structSize = sizeof(outputRecord);
    outputRecord.samples = &output;
    outputRecord.sampleCount = 1;
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process);
    process.frameCount = 1;
    process.inputs = &input;
    process.inputCount = 1;
    process.outputs = &outputRecord;
    process.outputCount = 1;
    require(
        loaded.instance->process(process)
            == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
        "a resolver call after prepare must fail closed on the callback path");
    require(
        !loaded.instance->shutdown(),
        "post-prepare resolver misuse must remain a recorded host violation");
}

void testBinaryResolverFailsClosedAfterPreparationWithoutCallbackLocking()
{
    Fixture fixture{
        TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_LATE_RESOLVER_FIXTURE_PATH,
        true};
    auto loaded = loadFixture(
        fixture, &fixture.grant, fixture.resourceBytes);
    require(loaded.ok(),
        "late Binary resolver fixture should prepare exactly once");

    double output = 0.0;
    TalasKernelAbiSignalV1 input{};
    input.structSize = sizeof(input);
    input.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    input.constantValue = 1.0;
    TalasKernelAbiOutputV1 outputRecord{};
    outputRecord.structSize = sizeof(outputRecord);
    outputRecord.samples = &output;
    outputRecord.sampleCount = 1;
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process);
    process.frameCount = 1;
    process.inputs = &input;
    process.inputCount = 1;
    process.outputs = &outputRecord;
    process.outputCount = 1;
    require(
        loaded.instance->process(process)
            == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
        "a Binary resolver call after prepare must fail closed in process");
    require(
        !loaded.instance->shutdown(),
        "late Binary resolver misuse must remain a host violation");
}
}

int main()
{
    testExactResourceMappingResolvesInPrepareAndReadsInProcess();
    testExactBinaryMappingResolvesInPrepareAndReadsInProcess();
    testResourceLoaderRejectsDetachedGrantAndPayloadBeforeLoad();
    testBinaryLoaderRejectsEveryDetachedTupleBeforeLoad();
    testResolverFailsClosedAfterPreparationWithoutCallbackLocking();
    testBinaryResolverFailsClosedAfterPreparationWithoutCallbackLocking();
    return 0;
}
