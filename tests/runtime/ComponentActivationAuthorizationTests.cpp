#include "runtime/component_activation/ComponentActivationAuthorization.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> failNextAllocation = false;
std::atomic<std::int64_t> failAllocationAfter = -1;

bool shouldFailAllocation()
{
    if (failNextAllocation.exchange(false, std::memory_order_relaxed))
        return true;
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

namespace talas::runtime::component_activation
{
struct ComponentActivationAuthorizationTestPeer
{
    [[nodiscard]] static std::optional<
        ComponentActivationRuntimeEvidenceAuthoringV1>
    authorRuntime(const ComponentActivationSealedSessionV1& session) noexcept
    {
        return ComponentActivationAuthorizationFactory::authorRuntimeEvidence(
            session);
    }

    [[nodiscard]] static const ComponentActivationImmutableResourceGrantV1*
    resourceGrant(const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.resourceGrant();
    }

    [[nodiscard]] static std::span<const std::uint8_t> resourceBytes(
        const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.resourceBytes();
    }

    [[nodiscard]] static const ComponentActivationAudioStreamGrantV1*
    audioStreamGrant(const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.audioStreamGrant();
    }

    [[nodiscard]] static std::span<const std::uint8_t> audioSourceBytes(
        const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.audioSourceBytes();
    }

    [[nodiscard]] static const ComponentActivationResourceSetGrantV1*
    resourceSetGrant(const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.resourceSetGrant();
    }

    [[nodiscard]] static std::span<
        const ComponentActivationResourceSetMemberV1>
    resourceSetMembers(const ComponentActivationLeaseV1& lease) noexcept
    {
        return lease.resourceSetMembers();
    }

    [[nodiscard]] static const ComponentActivationImmutableResourceGrantV1*
    resourceSetImmutableGrant(
        const ComponentActivationLeaseV1& lease,
        std::size_t index) noexcept
    {
        return lease.resourceSetImmutableGrant(index);
    }

    [[nodiscard]] static std::span<const std::uint8_t>
    resourceSetImmutableBytes(
        const ComponentActivationLeaseV1& lease,
        std::size_t index) noexcept
    {
        return lease.resourceSetImmutableBytes(index);
    }

    [[nodiscard]] static const ComponentActivationAudioStreamGrantV1*
    resourceSetAudioGrant(
        const ComponentActivationLeaseV1& lease,
        std::size_t index) noexcept
    {
        return lease.resourceSetAudioGrant(index);
    }

    [[nodiscard]] static std::span<const std::uint8_t>
    resourceSetAudioBytes(
        const ComponentActivationLeaseV1& lease,
        std::size_t index) noexcept
    {
        return lease.resourceSetAudioBytes(index);
    }

    [[nodiscard]] static ComponentActivationFrozenAudioSourceV1
    authorFrozenAudioSource(
        const talas::core::ResourceSnapshot& snapshot,
        std::vector<std::uint8_t> bytes)
    {
        auto data = std::make_shared<
            talas::runtime::detail::
                ComponentActivationFrozenAudioSourceDataV1>();
        data->id = snapshot.id;
        data->generation = snapshot.generation;
        data->sourceContentDigest =
            calculateComponentActivationByteDigestV1(bytes);
        data->bytes = std::move(bytes);
        data->channelCount = snapshot.channelCount;
        data->sampleRate = static_cast<std::uint32_t>(snapshot.sampleRate);
        data->frameCount = snapshot.frameCount;
        return ComponentActivationFrozenAudioSourceV1{std::move(data)};
    }
};
}

namespace
{
namespace activation = talas::runtime::component_activation;
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::array<std::uint8_t, componentTrustDigestBytes> digestFromHex(
    std::string_view text)
{
    require(
        text.size() == componentTrustDigestBytes * 2U,
        "digest fixture should have exact width");
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::runtime_error("digest fixture should be lowercase hex");
    };
    std::array<std::uint8_t, componentTrustDigestBytes> digest{};
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        digest[index] = static_cast<std::uint8_t>(
            (nibble(text[index * 2U]) << 4U)
            | nibble(text[index * 2U + 1U]));
    }
    return digest;
}

std::string digestToHex(
    const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2U);
    for (const auto byte : digest)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
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

void setU16(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void setU32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::uint32_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
}

std::vector<std::uint8_t> pcm16Wav(
    std::uint16_t channels = 2,
    std::uint32_t sampleRate = 48'000,
    std::uint32_t frameCount = 16'000)
{
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
    appendU16(bytes, static_cast<std::uint16_t>(channels * 2U));
    appendU16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(bytes, dataBytes);
    bytes.resize(44U + dataBytes);
    return bytes;
}

struct TemporaryAudioPath
{
    std::filesystem::path path;

    explicit TemporaryAudioPath(std::string_view suffix = ".wav")
    {
        static std::atomic<std::uint64_t> next{1};
        path = std::filesystem::path{"/private/tmp"}
            / ("talas-component-audio-authority-"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + "-" + std::to_string(next.fetch_add(1))
                + std::string{suffix});
    }

    ~TemporaryAudioPath()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

void writeBytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "Audio fixture should open its temporary path");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "Audio fixture should write every byte");
}

bundle::ComponentBundleSnapshot snapshot()
{
    constexpr std::string_view json = R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.talas.activation",
  "semanticVersion":"1.2.3",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":1,"apiMaximum":1},
  "roles":["realtime-processor"],
  "permissions":["realtime-audio"],
  "entry":{"path":"Payload/component.bin","bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.gain",
    "stateSchemaVersion":1,
    "inputs":[{"id":"in","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "outputs":[{"id":"out","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "parameters":[{"id":"gain","kind":"expression","domain":"scalar","choices":[]}]
  }],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Talas tests","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
    const auto parsed = bundle::parseComponentBundleManifest(json);
    require(parsed.ok(), "activation manifest fixture should parse");
    auto value = bundle::ComponentBundleSnapshot{};
    value.manifest = *parsed.manifest;
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        value.manifest);
    require(canonical.ok(), "activation manifest fixture should canonicalize");
    value.canonicalManifest = canonical.canonicalJson;
    value.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        value.manifest.entry.path,
        digestFromHex(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        {'a', 'b', 'c'},
        true,
    });
    value.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(value);
    require(
        bundle::validateComponentBundleSnapshotIdentity(value),
        "activation snapshot fixture should be exact");
    return value;
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
    value.runtimeSessionGeneration = 71;
    value.sharedStorageIdentity = 73;
    value.sharedStorageBytes = 64U * 1024U;
    value.runtimeSessionNonce.fill(0x77);
    return value;
}

probe::ComponentBundleProbeRequestV1 probeRequest(
    const bundle::ComponentBundleSnapshot& snapshot,
    const ComponentSignatureEvidenceView& signature)
{
    probe::ComponentBundleProbeRequestV1 request;
    request.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.bundleIdBytes = static_cast<std::uint32_t>(
        snapshot.manifest.bundleId.size());
    request.semanticVersionBytes = static_cast<std::uint32_t>(
        snapshot.manifest.semanticVersion.size());
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

probe::ComponentBundleProbeResultV1 cleanProbeResult(
    const probe::ComponentBundleProbeRequestV1& request)
{
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
    return result;
}

enum class AuthorizationFixtureProfile
{
    Stateless,
    ImmutableResource,
    AudioStream,
    MultipleResources,
    MaximumResources,
};

struct AuthorizationFixture
{
    bundle::ComponentBundleSnapshot snapshotValue = snapshot();
    ComponentSignatureEvidence signature;
    bundle::ComponentOwnerAuthorizationStore owner;
    probe::ComponentBundleProbeEvidenceAuthoringV1 probeAuthoring;
    activation::ComponentActivationRuntimeEvidenceAuthoringV1 runtimeAuthoring;
    activation::ComponentActivationSealedSessionV1 sealed = sealedSession();
    std::vector<activation::ComponentActivationStaticParameterV1>
        staticParameters{1};
    talas::core::ResourceSnapshotPtr resourceSnapshotValue;
    std::optional<activation::ComponentActivationFrozenAudioSourceV1>
        frozenAudioSource;
    std::vector<talas::core::ResourceSnapshotPtr> resourceSetSnapshotValues;
    std::vector<
        std::unique_ptr<activation::ComponentActivationFrozenAudioSourceV1>>
        resourceSetAudioSources;
    std::vector<activation::ComponentActivationResourceSetInputV1>
        resourceSetInputs;
    std::unique_ptr<TemporaryAudioPath> resourceSetAudioPath;
    activation::ComponentActivationAuthorizationInputsV1 inputs;

    AuthorizationFixture()
        : AuthorizationFixture(AuthorizationFixtureProfile::Stateless)
    {
    }

    explicit AuthorizationFixture(bool resourceProfile)
        : AuthorizationFixture(
              resourceProfile
                  ? AuthorizationFixtureProfile::ImmutableResource
                  : AuthorizationFixtureProfile::Stateless)
    {
    }

    explicit AuthorizationFixture(AuthorizationFixtureProfile profile)
    {
        const auto resourceProfile =
            profile != AuthorizationFixtureProfile::Stateless;
        const auto audioStreamProfile =
            profile == AuthorizationFixtureProfile::AudioStream;
        const auto multipleResourceProfile =
            profile == AuthorizationFixtureProfile::MultipleResources
            || profile == AuthorizationFixtureProfile::MaximumResources;
        const auto maximumResourceProfile =
            profile == AuthorizationFixtureProfile::MaximumResources;
        if (resourceProfile)
        {
            if (!audioStreamProfile || multipleResourceProfile)
            {
                snapshotValue.manifest.declaredRoles |= componentRole(
                    ComponentExecutionRole::ImmutableResourceConsumer);
                snapshotValue.manifest.requestedPermissions |=
                    componentPermission(
                        ComponentExecutionPermission::ImmutableResourceRead);
            }
            if (audioStreamProfile || multipleResourceProfile)
            {
                snapshotValue.manifest.declaredRoles |= componentRole(
                    ComponentExecutionRole::BoundedAudioStreamConsumer);
                snapshotValue.manifest.requestedPermissions |=
                    componentPermission(
                        ComponentExecutionPermission::BoundedAudioStreamRead);
                snapshotValue.manifest.minimumEngineApiVersion =
                    multipleResourceProfile
                        ? TALAS_KERNEL_API_VERSION_V4
                        : TALAS_KERNEL_API_VERSION_V3;
                snapshotValue.manifest.maximumEngineApiVersion =
                    snapshotValue.manifest.minimumEngineApiVersion;
            }
            auto& parameters =
                snapshotValue.manifest.nodes.front().parameters;
            if (multipleResourceProfile)
            {
                if (maximumResourceProfile)
                {
                    parameters.clear();
                    for (std::size_t index = 0;
                         index
                            < activation::
                                maximumComponentActivationResourceSetMembersV1;
                         ++index)
                    {
                        parameters.push_back({
                            "resource" + std::to_string(index),
                            talas::core::ParameterKind::Resource,
                            talas::expr::ValueDomain::Scalar,
                            {},
                        });
                    }
                }
                else
                {
                    parameters = {
                        {"table", talas::core::ParameterKind::Resource,
                         talas::expr::ValueDomain::Scalar, {}},
                        {"binary", talas::core::ParameterKind::Resource,
                         talas::expr::ValueDomain::Scalar, {}},
                        {"audio", talas::core::ParameterKind::Resource,
                         talas::expr::ValueDomain::Scalar, {}},
                    };
                }
            }
            else
            {
                auto& parameter = parameters.front();
                parameter.id = audioStreamProfile ? "audio" : "table";
                parameter.kind = talas::core::ParameterKind::Resource;
                parameter.domain = talas::expr::ValueDomain::Scalar;
            }
            const auto canonical =
                bundle::writeCanonicalComponentBundleManifest(
                    snapshotValue.manifest);
            require(
                canonical.ok(),
                "resource activation manifest should canonicalize");
            snapshotValue.canonicalManifest = canonical.canonicalJson;
            snapshotValue.contentDigest =
                bundle::calculateComponentBundleSnapshotContentDigest(
                    snapshotValue);
            require(
                bundle::validateComponentBundleSnapshotIdentity(snapshotValue),
                "resource activation snapshot should remain exact");
            if (!audioStreamProfile || multipleResourceProfile)
            {
                sealed.supportedPermissions |= componentPermission(
                    ComponentExecutionPermission::ImmutableResourceRead);
            }
            if (audioStreamProfile || multipleResourceProfile)
            {
                sealed.supportedPermissions |= componentPermission(
                    ComponentExecutionPermission::BoundedAudioStreamRead);
            }
        }

        ComponentSignatureEvidenceView signatureView;
        signatureView.state = ComponentSignatureState::Verified;
        signatureView.contentDigest = snapshotValue.contentDigest;
        signatureView.signerIdentityDigest.fill(0x42);
        signatureView.profileVersion = currentComponentSignatureProfileVersion;
        signatureView.hasContentDigest = true;
        signatureView.hasSignerIdentity = true;
        signature = bundle::ComponentBundleSignatureEvidenceFactory::make(
            signatureView);

        ComponentApprovalFingerprint candidate;
        candidate.bundleId = snapshotValue.manifest.bundleId;
        candidate.semanticVersion = snapshotValue.manifest.semanticVersion;
        candidate.contentDigest = snapshotValue.contentDigest;
        candidate.signerIdentityDigest = signatureView.signerIdentityDigest;
        candidate.requestedPermissions =
            snapshotValue.manifest.requestedPermissions;
        candidate.policyVersion = currentComponentTrustPolicyVersion;
        candidate.hasContentDigest = true;
        candidate.hasSignerIdentity = true;
        require(
            owner.recordOwnerAction(
                     candidate,
                     bundle::ComponentOwnerAuthorizationAction::Approve,
                     owner.generation())
                .ok(),
            "activation owner approval fixture should apply");

        const auto request = probeRequest(snapshotValue, signatureView);
        auto authoredProbe = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(cleanProbeResult(request), request);
        require(authoredProbe.has_value(), "clean probe fixture should author");
        probeAuthoring = std::move(*authoredProbe);

        auto authoredRuntime =
            activation::ComponentActivationAuthorizationTestPeer::
                authorRuntime(sealed);
        require(
            authoredRuntime.has_value(),
            "sealed runtime fixture should author opaque evidence");
        runtimeAuthoring = std::move(*authoredRuntime);

        if (multipleResourceProfile)
        {
            if (maximumResourceProfile)
            {
                const auto count = activation::
                    maximumComponentActivationResourceSetMembersV1;
                staticParameters.resize(count);
                resourceSetSnapshotValues.reserve(count);
                resourceSetInputs.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    const auto parameterId =
                        "resource" + std::to_string(index);
                    const auto resourceId =
                        "Tables/Maximum-" + std::to_string(index);
                    auto& parameter = staticParameters[index];
                    parameter.kind = static_cast<std::uint32_t>(
                        activation::
                            ComponentActivationStaticParameterKindV1::
                                Resource);
                    parameter.idBytes = parameterId.size();
                    parameter.textBytes = resourceId.size();
                    std::copy(
                        parameterId.begin(),
                        parameterId.end(),
                        parameter.id.begin());
                    std::copy(
                        resourceId.begin(),
                        resourceId.end(),
                        parameter.text.begin());
                    talas::core::ResourceSnapshot table;
                    table.id = resourceId;
                    table.kind = talas::core::ResourceSnapshotKind::Table;
                    table.generation = index + 1U;
                    table.contentHash = std::string(64, '6');
                    table.resolvedPath =
                        "/private/project/" + resourceId + ".table";
                    table.tableSamples = {
                        static_cast<double>(index + 1U)};
                    resourceSetSnapshotValues.push_back(
                        std::make_shared<
                            const talas::core::ResourceSnapshot>(
                            std::move(table)));
                    resourceSetInputs.push_back(
                        {resourceSetSnapshotValues.back(), nullptr});
                }
            }
            else
            {
            staticParameters.resize(3);
            const auto configureParameter = [&](std::size_t index,
                                                std::string_view parameterId,
                                                std::string_view resourceId) {
                auto& parameter = staticParameters[index];
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Resource);
                parameter.idBytes = parameterId.size();
                parameter.textBytes = resourceId.size();
                std::copy(
                    parameterId.begin(),
                    parameterId.end(),
                    parameter.id.begin());
                std::copy(
                    resourceId.begin(),
                    resourceId.end(),
                    parameter.text.begin());
            };
            resourceSetSnapshotValues.reserve(3);
            resourceSetAudioSources.reserve(1);
            resourceSetInputs.reserve(3);

            configureParameter(0, "table", "Tables/Curve-A");
            talas::core::ResourceSnapshot table;
            table.id = "Tables/Curve-A";
            table.kind = talas::core::ResourceSnapshotKind::Table;
            table.generation = 7;
            table.contentHash = std::string(64, '6');
            table.resolvedPath = "/private/project/Tables/Curve-A.table";
            table.tableSamples = {1.0, -0.0, 0.5};
            resourceSetSnapshotValues.push_back(
                std::make_shared<const talas::core::ResourceSnapshot>(
                    std::move(table)));

            configureParameter(1, "binary", "Binary/Blob-A");
            talas::core::ResourceSnapshot binary;
            binary.id = "Binary/Blob-A";
            binary.kind = talas::core::ResourceSnapshotKind::Binary;
            binary.generation = 11;
            binary.resolvedPath = "/private/project/Assets/Blob-A.bin";
            binary.binaryBytes = {0x00, 0x7f, 0x80, 0xff};
            binary.contentHash = digestToHex(
                activation::calculateComponentActivationByteDigestV1(
                    binary.binaryBytes));
            resourceSetSnapshotValues.push_back(
                std::make_shared<const talas::core::ResourceSnapshot>(
                    std::move(binary)));

            configureParameter(2, "audio", "Audio/Voice-A");
            const auto audioBytes = pcm16Wav();
            resourceSetAudioPath = std::make_unique<TemporaryAudioPath>();
            writeBytes(resourceSetAudioPath->path, audioBytes);
            talas::core::ResourceSnapshot audio;
            audio.id = "Audio/Voice-A";
            audio.kind = talas::core::ResourceSnapshotKind::Audio;
            audio.generation = 13;
            audio.contentHash =
                talas::project::calculateProjectAssetSha256(audioBytes);
            audio.resolvedPath = resourceSetAudioPath->path;
            audio.channelCount = 2;
            audio.sampleRate = 48'000.0;
            audio.frameCount = 16'000;
            audio.encoding = "pcm16-wav";
            auto frozen =
                activation::ComponentActivationAudioSourceFreezerV1::freeze(
                    audio);
            require(
                frozen.ok(),
                "multiple-resource Audio fixture should freeze");
            resourceSetAudioSources.push_back(std::make_unique<
                activation::ComponentActivationFrozenAudioSourceV1>(
                std::move(*frozen.source)));
            resourceSetSnapshotValues.push_back(
                std::make_shared<const talas::core::ResourceSnapshot>(
                    std::move(audio)));

            resourceSetInputs.push_back(
                {resourceSetSnapshotValues[0], nullptr});
            resourceSetInputs.push_back(
                {resourceSetSnapshotValues[1], nullptr});
            resourceSetInputs.push_back(
                {resourceSetSnapshotValues[2],
                 resourceSetAudioSources.front().get()});
            }
        }
        else if (resourceProfile)
        {
            staticParameters.front().kind = static_cast<std::uint32_t>(
                activation::ComponentActivationStaticParameterKindV1::Resource);
            const std::string_view parameterId = audioStreamProfile
                ? "audio"
                : "table";
            staticParameters.front().idBytes = parameterId.size();
            std::copy(
                parameterId.begin(),
                parameterId.end(),
                staticParameters.front().id.begin());
            const std::string_view resourceId = audioStreamProfile
                ? "Audio/Voice-A"
                : "Tables/Curve-A";
            staticParameters.front().textBytes = resourceId.size();
            std::copy(
                resourceId.begin(),
                resourceId.end(),
                staticParameters.front().text.begin());
            talas::core::ResourceSnapshot resource;
            resource.id = resourceId;
            resource.kind = audioStreamProfile
                ? talas::core::ResourceSnapshotKind::Audio
                : talas::core::ResourceSnapshotKind::Table;
            resource.generation = 7;
            if (audioStreamProfile)
            {
                resource.channelCount = 2;
                resource.sampleRate = 48'000.0;
                resource.frameCount = 16'000;
                resource.encoding = "pcm16-wav";
            }
            else
            {
                resource.contentHash = std::string(64, '6');
                resource.resolvedPath =
                    "/private/project/Tables/Curve-A.table";
                resource.tableSamples = {1.0, -0.0, 0.5};
            }
            resourceSnapshotValue = std::make_shared<
                const talas::core::ResourceSnapshot>(std::move(resource));
        }
        else
        {
            staticParameters.front().kind = static_cast<std::uint32_t>(
                activation::ComponentActivationStaticParameterKindV1::Scalar);
            staticParameters.front().idBytes = 4;
            std::copy_n(
                "gain",
                staticParameters.front().idBytes,
                staticParameters.front().id.begin());
            staticParameters.front().scalarValue = 0.5;
        }

        inputs.snapshot = &snapshotValue;
        inputs.signature = signature;
        inputs.ownerAuthorization = owner.authorizationEvidence();
        inputs.probe = probeAuthoring.evidence;
        inputs.runtime = runtimeAuthoring.evidence;
        inputs.validationNonce = request.validationNonce;
        inputs.authorizationNonce.fill(0xa5);
        inputs.descriptorId = "org.talas.activation.gain";
        inputs.artifactGeneration = request.artifactGeneration;
        inputs.activationGeneration = 83;
        inputs.sampleRate = 48'000;
        inputs.blockFrames = 256;
        inputs.inputChannelCount = 1;
        inputs.outputChannelCount = 1;
        inputs.staticParameters = staticParameters;
        inputs.resourceSnapshot = resourceSnapshotValue;
        inputs.resourceSetInputs = resourceSetInputs;
    }

    ComponentApprovalFingerprint candidate() const
    {
        ComponentApprovalFingerprint value;
        value.bundleId = snapshotValue.manifest.bundleId;
        value.semanticVersion = snapshotValue.manifest.semanticVersion;
        value.contentDigest = snapshotValue.contentDigest;
        value.signerIdentityDigest.fill(0x42);
        value.requestedPermissions =
            snapshotValue.manifest.requestedPermissions;
        value.policyVersion = currentComponentTrustPolicyVersion;
        value.hasContentDigest = true;
        value.hasSignerIdentity = true;
        return value;
    }

    std::shared_ptr<talas::core::ResourceSnapshot> useAudioSource(
        const std::filesystem::path& path,
        std::span<const std::uint8_t> bytes)
    {
        require(
            resourceSnapshotValue != nullptr
                && resourceSnapshotValue->kind
                    == talas::core::ResourceSnapshotKind::Audio,
            "Audio source fixture requires the Audio stream profile");
        auto resource = std::make_shared<talas::core::ResourceSnapshot>(
            *resourceSnapshotValue);
        resource->resolvedPath = path;
        resource->contentHash =
            talas::project::calculateProjectAssetSha256(bytes);
        auto frozen =
            activation::ComponentActivationAudioSourceFreezerV1::freeze(
                *resource);
        require(frozen.ok(), "valid Audio source fixture should freeze");
        frozenAudioSource = std::move(*frozen.source);
        resourceSnapshotValue = resource;
        inputs.resourceSnapshot = resourceSnapshotValue;
        inputs.audioSource = &*frozenAudioSource;
        return resource;
    }

    std::shared_ptr<talas::core::ResourceSnapshot> useBinaryResource(
        std::vector<std::uint8_t> bytes)
    {
        require(
            resourceSnapshotValue != nullptr,
            "Binary resource fixture requires the resource profile");
        auto resource = std::make_shared<talas::core::ResourceSnapshot>(
            *resourceSnapshotValue);
        resource->kind = talas::core::ResourceSnapshotKind::Binary;
        resource->resolvedPath = "/private/project/Assets/Curve-A.bin";
        resource->tableSamples.clear();
        resource->binaryBytes = std::move(bytes);
        resource->contentHash = digestToHex(
            activation::calculateComponentActivationByteDigestV1(
                resource->binaryBytes));
        resourceSnapshotValue = resource;
        inputs.resourceSnapshot = resourceSnapshotValue;
        return resource;
    }

    std::shared_ptr<talas::core::ResourceSnapshot>
    replaceResourceSetMemberWithAudio(
        std::size_t index,
        std::vector<std::uint8_t> bytes)
    {
        require(
            index < resourceSetInputs.size(),
            "replacement Audio member index should be in range");
        const auto shape =
            activation::parseComponentActivationPcm16WaveV1(bytes);
        require(
            shape.has_value(),
            "replacement Audio member bytes should be canonical PCM16 WAV");
        auto resource = std::make_shared<talas::core::ResourceSnapshot>(
            *resourceSetInputs[index].snapshot);
        resource->kind = talas::core::ResourceSnapshotKind::Audio;
        resource->contentHash =
            talas::project::calculateProjectAssetSha256(bytes);
        resource->resolvedPath =
            "/private/project/Audio/Replacement-"
            + std::to_string(index) + ".wav";
        resource->tableSamples.clear();
        resource->binaryBytes.clear();
        resource->channelCount = shape->channelCount;
        resource->sampleRate = shape->sampleRate;
        resource->frameCount = shape->frameCount;
        resource->encoding = "pcm16-wav";
        auto source = std::make_unique<
            activation::ComponentActivationFrozenAudioSourceV1>(
            activation::ComponentActivationAuthorizationTestPeer::
                authorFrozenAudioSource(*resource, std::move(bytes)));
        const auto* sourcePointer = source.get();
        resourceSetAudioSources.push_back(std::move(source));
        resourceSetSnapshotValues[index] = resource;
        resourceSetInputs[index] = {resource, sourcePointer};
        inputs.resourceSetInputs = resourceSetInputs;
        return resource;
    }
};

void testAtomicMultipleResourceAuthorityOwnsOneMixedSet()
{
    AuthorizationFixture fixture{
        AuthorizationFixtureProfile::MultipleResources};
    auto mutableBinary = std::make_shared<talas::core::ResourceSnapshot>(
        *fixture.resourceSetInputs[1].snapshot);
    fixture.resourceSetInputs[1].snapshot = mutableBinary;
    fixture.inputs.resourceSetInputs = fixture.resourceSetInputs;
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(
        result.ok(),
        "one exact Table/Binary/Audio set should authorize, code "
            + std::to_string(static_cast<unsigned>(result.code)));
    mutableBinary->generation = 99;
    mutableBinary->binaryBytes.assign(8, 0x55);
    mutableBinary->contentHash.assign(64, '0');
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active()
            && !result.authority->consume().has_value(),
        "the complete resource set should issue exactly one live lease");

    const auto* summary =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetGrant(*lease);
    const auto members =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetMembers(*lease);
    require(
        summary != nullptr && summary->memberCount == 3
            && summary->audioStreamCount == 1 && members.size() == 3
            && activation::validateComponentActivationResourceSetGrantV1(
                   *summary,
                   *lease->record(),
                   fixture.sealed)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid
            && activation::validateComponentActivationResourceSetMembersV1(
                   *summary, members)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the lease should retain one exact valid set summary and member list");
    require(
        members[0].memberKind
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationResourceSetMemberKindV1::
                        Table)
            && members[1].memberKind
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationResourceSetMemberKindV1::
                        Binary)
            && members[2].memberKind
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationResourceSetMemberKindV1::
                        Audio)
            && members[0].memberOrdinal == 0
            && members[1].memberOrdinal == 1
            && members[2].memberOrdinal == 2,
        "set order must be authored from canonical manifest parameter order");

    const auto* table =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetImmutableGrant(*lease, 0);
    const auto tableBytes =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetImmutableBytes(*lease, 0);
    const auto* binary =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetImmutableGrant(*lease, 1);
    const auto binaryBytes =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetImmutableBytes(*lease, 1);
    const auto* audio =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetAudioGrant(*lease, 2);
    const auto audioBytes =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetAudioBytes(*lease, 2);
    require(
        table != nullptr && binary != nullptr && audio != nullptr
            && tableBytes.size() == 3U * sizeof(double)
            && std::ranges::equal(
                binaryBytes,
                fixture.resourceSetSnapshotValues[1]->binaryBytes)
            && audioBytes.size() == 44U + 16'000U * 2U * 2U
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetAudioGrant(*lease, 0)
                == nullptr
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetImmutableGrant(*lease, 2)
                == nullptr,
        "each set slot should retain only its exact closed grant and bytes");
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            *table,
            *lease->record(),
            fixture.sealed,
            fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid
            && activation::validateComponentActivationImmutableResourceGrantV1(
                   *binary,
                   *lease->record(),
                   fixture.sealed,
                   fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid
            && activation::validateComponentActivationAudioStreamGrantV1(
                   *audio,
                   *lease->record(),
                   fixture.sealed,
                   fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "every nested grant should remain valid under the combined v4 authority");
    for (const auto bytes : {tableBytes, binaryBytes, audioBytes})
    {
        require(
            std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()}
                    .find("/private/project")
                == std::string_view::npos,
            "the resource-set lease must never retain a project path");
    }
}

void testMultipleResourceAuthorityRejectsEveryPartialSet()
{
    const auto rejects = [](auto mutate,
                            activation::ComponentActivationAuthorizationCodeV1
                                expected,
                            std::string_view message) {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        auto inputs = fixture.inputs;
        auto setInputs = fixture.resourceSetInputs;
        mutate(inputs, setInputs, fixture);
        inputs.resourceSetInputs = setInputs;
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(inputs);
        require(
            !result.ok() && result.code == expected,
            std::string(message));
    };
    rejects(
        [](auto&, auto& setInputs, auto&) { setInputs.resize(2); },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "a missing set member must reject without publishing a partial lease");
    rejects(
        [](auto&, auto& setInputs, auto&) {
            std::swap(setInputs[0], setInputs[1]);
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "caller-selected resource ordering must reject");
    rejects(
        [](auto&, auto& setInputs, auto&) { setInputs[1] = setInputs[0]; },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "one snapshot cannot satisfy two canonical members");
    rejects(
        [](auto&, auto& setInputs, auto&) {
            setInputs.push_back(setInputs.front());
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "an extra set member must reject");
    rejects(
        [](auto&, auto& setInputs, auto&) {
            setInputs[2].audioSource = nullptr;
        },
        activation::ComponentActivationAuthorizationCodeV1::AudioSourceInvalid,
        "an Audio member without its exact frozen source must reject");
    rejects(
        [](auto& inputs, auto&, auto&) {
            inputs.resourceSnapshot = std::make_shared<
                const talas::core::ResourceSnapshot>();
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "legacy and resource-set inputs cannot be mixed");
    rejects(
        [](auto&, auto& setInputs, auto&) {
            auto changed = std::make_shared<talas::core::ResourceSnapshot>(
                *setInputs[1].snapshot);
            changed->generation = 0;
            setInputs[1].snapshot = std::move(changed);
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "a stale member generation must reject the complete set");
    rejects(
        [](auto&, auto& setInputs, auto&) {
            auto changed = std::make_shared<talas::core::ResourceSnapshot>(
                *setInputs[1].snapshot);
            changed->binaryBytes.front() ^= 0xffU;
            setInputs[1].snapshot = std::move(changed);
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "same-sized member mutation must reject against source identity");
    rejects(
        [](auto& inputs, auto& setInputs, auto& fixture) {
            const std::string_view duplicateId = "Tables/Curve-A";
            auto& parameter = fixture.staticParameters[1];
            parameter.text = {};
            parameter.textBytes = duplicateId.size();
            std::copy(
                duplicateId.begin(),
                duplicateId.end(),
                parameter.text.begin());
            inputs.staticParameters = fixture.staticParameters;
            auto changed = std::make_shared<talas::core::ResourceSnapshot>(
                *setInputs[0].snapshot);
            setInputs[1].snapshot = std::move(changed);
        },
        activation::ComponentActivationAuthorizationCodeV1::ResourceInvalid,
        "duplicate resource IDs must reject even across distinct parameters");

    AuthorizationFixture allocation{
        AuthorizationFixtureProfile::MultipleResources};
    failNextAllocation.store(true, std::memory_order_relaxed);
    const auto failed =
        activation::ComponentActivationAuthorizationFactory::make(
            allocation.inputs);
    require(
        !failed.ok()
            && failed.code
                == activation::ComponentActivationAuthorizationCodeV1::
                    AllocationFailed,
        "allocation failure must expose no partially authored set authority");

    bool reachedCompleteAuthority = false;
    for (std::int64_t failureIndex = 0; failureIndex < 256; ++failureIndex)
    {
        failAllocationAfter.store(failureIndex, std::memory_order_relaxed);
        auto attempt =
            activation::ComponentActivationAuthorizationFactory::make(
                allocation.inputs);
        failAllocationAfter.store(-1, std::memory_order_relaxed);
        if (attempt.ok())
        {
            reachedCompleteAuthority = true;
            break;
        }
        require(
            !attempt.ok() && !attempt.authority.has_value(),
            "every injected allocation seam must expose no partial authority");
    }
    require(
        reachedCompleteAuthority,
        "allocation-seam sweep should eventually reach a complete authority");
}

void testMultipleResourceLeaseRevokesTheCompleteSetAtomically()
{
    AuthorizationFixture fixture{
        AuthorizationFixtureProfile::MultipleResources};
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "set revocation fixture should authorize");
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "set revocation fixture should issue one lease");
    require(
        fixture.owner.revokeOwnerAction(
                fixture.candidate(), fixture.owner.generation())
            .ok(),
        "set owner revocation should apply");
    require(
        !lease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetGrant(*lease)
                == nullptr
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetMembers(*lease)
                .empty()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetImmutableBytes(*lease, 0)
                .empty()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetImmutableBytes(*lease, 1)
                .empty()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetAudioBytes(*lease, 2)
                .empty(),
        "one live-source revocation must hide every set record and byte span");
}

void testMultipleResourceSetSharesEveryLiveBoundary()
{
    {
        AuthorizationFixture denied{
            AuthorizationFixtureProfile::MultipleResources};
        require(
            denied.owner.replaceOwnerAction(
                    denied.candidate(),
                    bundle::ComponentOwnerAuthorizationAction::Deny,
                    denied.owner.generation())
                .ok(),
            "set denial fixture should replace approval");
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                denied.inputs);
        require(
            !result.ok()
                && result.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        OwnerNotApproved,
            "an exact owner denial must expose no set authority");
    }
    {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        auto lease = result.authority->consume();
        fixture.probeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceSetMembers(*lease)
                    .empty(),
            "probe revocation must hide the complete resource set");
    }
    {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        auto lease = result.authority->consume();
        fixture.runtimeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceSetMembers(*lease)
                    .empty(),
            "runtime revocation must hide the complete resource set");
    }
    {
        std::optional<activation::ComponentActivationLeaseV1> lease;
        {
            auto fixture = std::make_unique<AuthorizationFixture>(
                AuthorizationFixtureProfile::MultipleResources);
            auto result =
                activation::ComponentActivationAuthorizationFactory::make(
                    fixture->inputs);
            require(
                result.ok(),
                "set store-destruction fixture should authorize");
            lease = result.authority->consume();
        }
        require(
            lease.has_value() && !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceSetMembers(*lease)
                    .empty(),
            "authority-source destruction must hide the complete set");
    }
}

void testMultipleResourceStageLeaseLinearizesRevocation()
{
    AuthorizationFixture fixture{
        AuthorizationFixtureProfile::MultipleResources};
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "set stage-admission fixture should authorize");
    auto lease = result.authority->consume();
    auto stageLease = lease->tryAcquireStageLease();
    require(
        stageLease.has_value() && stageLease->active(),
        "the complete set should acquire one stage-admission lease");

    std::atomic<bool> revokeEntered{false};
    std::atomic<bool> revokeCompleted{false};
    std::thread revokeThread([&] {
        revokeEntered.store(true, std::memory_order_release);
        const auto revoked = fixture.owner.revokeOwnerAction(
            fixture.candidate(), fixture.owner.generation());
        revokeCompleted.store(revoked.ok(), std::memory_order_release);
    });
    while (!revokeEntered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(
        !revokeCompleted.load(std::memory_order_acquire)
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetMembers(*lease)
                .size()
                == 3,
        "revocation must not split a set during admitted graph publication");
    stageLease.reset();
    revokeThread.join();
    require(
        revokeCompleted.load(std::memory_order_acquire) && !lease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceSetMembers(*lease)
                .empty(),
        "revocation must hide the full set after stage admission releases");
}

void testMultipleResourceAudioAndAggregateBoundaries()
{
    {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        fixture.replaceResourceSetMemberWithAudio(1, pcm16Wav());
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            result.ok(),
            "the exact two-Audio member boundary should authorize");
        auto lease = result.authority->consume();
        const auto* summary =
            activation::ComponentActivationAuthorizationTestPeer::
                resourceSetGrant(*lease);
        require(
            summary != nullptr && summary->audioStreamCount == 2,
            "the authority must retain the exact two-Audio aggregate count");
    }
    {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        fixture.replaceResourceSetMemberWithAudio(0, pcm16Wav());
        fixture.replaceResourceSetMemberWithAudio(1, pcm16Wav());
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            !result.ok()
                && result.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        ResourceInvalid,
            "a third Audio member must reject before set publication");
    }
    {
        AuthorizationFixture fixture{
            AuthorizationFixtureProfile::MultipleResources};
        constexpr auto tableBytes = 3U * sizeof(double);
        constexpr auto perAudioBytes =
            (activation::maximumComponentActivationResourceSetSourceBytesV1
                - tableBytes)
            / 2U;
        static_assert(
            tableBytes + 2U * perAudioBytes
            == activation::maximumComponentActivationResourceSetSourceBytesV1);
        constexpr auto perAudioFrames = (perAudioBytes - 44U) / 2U;
        static_assert(44U + perAudioFrames * 2U == perAudioBytes);
        fixture.replaceResourceSetMemberWithAudio(
            1,
            pcm16Wav(1, 48'000, perAudioFrames));
        fixture.replaceResourceSetMemberWithAudio(
            2,
            pcm16Wav(1, 48'000, perAudioFrames));
        auto exact =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            exact.ok(),
            "the exact 64 MiB aggregate source boundary should authorize");
        auto lease = exact.authority->consume();
        const auto* summary =
            activation::ComponentActivationAuthorizationTestPeer::
                resourceSetGrant(*lease);
        require(
            summary != nullptr
                && summary->aggregateSourceBytes
                    == activation::
                        maximumComponentActivationResourceSetSourceBytesV1,
            "the lease must retain the exact 64 MiB aggregate source budget");
        lease->revoke();

        fixture.replaceResourceSetMemberWithAudio(
            2,
            pcm16Wav(1, 48'000, perAudioFrames + 1U));
        const auto over =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            !over.ok()
                && over.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        ResourceInvalid,
            "one source frame beyond 64 MiB must reject before copying");
    }
}

void testMultipleResourceMemberCountBoundary()
{
    AuthorizationFixture fixture{
        AuthorizationFixtureProfile::MaximumResources};
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(
        result.ok(),
        "the exact eight-member resource-set boundary should authorize");
    auto lease = result.authority->consume();
    const auto* summary =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetGrant(*lease);
    const auto members =
        activation::ComponentActivationAuthorizationTestPeer::
            resourceSetMembers(*lease);
    require(
        summary != nullptr
            && summary->memberCount
                == activation::maximumComponentActivationResourceSetMembersV1
            && summary->audioStreamCount == 0
            && members.size()
                == activation::maximumComponentActivationResourceSetMembersV1
            && members.back().memberOrdinal
                == activation::maximumComponentActivationResourceSetMembersV1
                    - 1U,
        "the complete eight-member canonical order must remain visible");
}

void testAudioSourceFreezerAndAuthorityOwnExactPathlessBytes()
{
    TemporaryAudioPath sourcePath;
    const auto expectedBytes = pcm16Wav();
    writeBytes(sourcePath.path, expectedBytes);
    AuthorizationFixture fixture{AuthorizationFixtureProfile::AudioStream};
    auto mutableAlias = fixture.useAudioSource(
        sourcePath.path, expectedBytes);
    auto editedBytes = expectedBytes;
    editedBytes.back() = 0x7f;
    writeBytes(sourcePath.path, editedBytes);
    require(
        std::filesystem::remove(sourcePath.path),
        "Audio source fixture should allow removal after freezing");

    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(
        result.ok(),
        "file edit/removal after exact freezing must not alter authority bytes");

    mutableAlias->generation = 99;
    mutableAlias->contentHash.assign(64, '0');
    mutableAlias->resolvedPath = "/private/project/Audio/Mutated.wav";
    mutableAlias->channelCount = 1;
    mutableAlias->sampleRate = 1.0;
    mutableAlias->frameCount = 1;
    mutableAlias->encoding = "mutated";

    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "frozen Audio authority should issue one active lease");
    require(
        !result.authority->consume().has_value(),
        "frozen Audio authority must remain one-use");
    const auto* grant =
        activation::ComponentActivationAuthorizationTestPeer::audioStreamGrant(
            *lease);
    const auto bytes =
        activation::ComponentActivationAuthorizationTestPeer::audioSourceBytes(
            *lease);
    require(
        grant != nullptr
            && grant->resourceGeneration == 7
            && grant->sourceByteCount == expectedBytes.size()
            && grant->sourceChannelCount == 2
            && grant->sourceSampleRate == 48'000
            && grant->sourceFrameCount == 16'000
            && grant->sourceEncoding
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationAudioSourceEncodingV1::
                        Pcm16Wave)
            && grant->signerIdentityDigest
                == lease->record()->signerIdentityDigest
            && grant->sourceContentDigest
                == activation::calculateComponentActivationByteDigestV1(
                    expectedBytes)
            && std::ranges::equal(bytes, expectedBytes)
            && activation::validateComponentActivationAudioStreamGrantV1(
                   *grant,
                   *lease->record(),
                   fixture.sealed,
                   fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the lease must retain exact raw bytes, media identity, and grant binding");
    require(
        std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()}
                .find("/private/") == std::string_view::npos,
        "the frozen Audio authority must never serialize a project path");

    lease->revoke();
    require(
        !lease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioStreamGrant(*lease)
                == nullptr
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioSourceBytes(*lease)
                .empty(),
        "Audio grant and bytes must disappear with lease revocation");
}

void testAudioSourceFreezerRejectsPathMediaAndMutationFailures()
{
    AuthorizationFixture fixture{AuthorizationFixtureProfile::AudioStream};
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>(
        *fixture.resourceSnapshotValue);

    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                SnapshotInvalid,
        "an Audio snapshot without a path/hash must reject");

    TemporaryAudioPath missing;
    snapshot->resolvedPath = missing.path;
    snapshot->contentHash.assign(64, '0');
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                Unavailable,
        "a missing Audio source must reject");

    TemporaryAudioPath directory{".dir"};
    require(
        std::filesystem::create_directory(directory.path),
        "Audio directory fixture should be created");
    snapshot->resolvedPath = directory.path;
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                NotRegularFile,
        "a non-regular Audio source must reject");

    TemporaryAudioPath validPath;
    auto bytes = pcm16Wav();
    writeBytes(validPath.path, bytes);
    snapshot->resolvedPath = validPath.path;
    snapshot->contentHash = talas::project::calculateProjectAssetSha256(bytes);

    failNextAllocation.store(true, std::memory_order_relaxed);
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                AllocationFailed,
        "Audio source allocation failure must reject without escaping noexcept");

    TemporaryAudioPath symbolicPath{".link"};
    std::error_code symlinkError;
    std::filesystem::create_symlink(
        validPath.path, symbolicPath.path, symlinkError);
    require(
        !symlinkError,
        "Audio symlink fixture should be created");
    snapshot->resolvedPath = symbolicPath.path;
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                NotRegularFile,
        "a symlink Audio source must reject before opening");
    snapshot->resolvedPath = validPath.path;

    auto cancelled = activation::ComponentActivationAudioSourceFreezerV1::freeze(
        *snapshot, [] { return true; });
    require(
        cancelled.code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::Cancelled,
        "Audio freezing must be cancellable before acquisition");

    writeBytes(
        validPath.path,
        std::span<const std::uint8_t>{bytes}.first(
            activation::minimumComponentActivationAudioSourceBytesV1 - 1));
    snapshot->contentHash = talas::project::calculateProjectAssetSha256(
        std::span<const std::uint8_t>{bytes}.first(
            activation::minimumComponentActivationAudioSourceBytesV1 - 1));
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                MediaInvalid,
        "a short Audio source must reject before allocation or parsing");

    writeBytes(validPath.path, std::span<const std::uint8_t>{bytes}.first(1));
    std::filesystem::resize_file(
        validPath.path,
        activation::maximumComponentActivationAudioSourceBytesV1 + 1);
    snapshot->contentHash.assign(64, '0');
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                MediaInvalid,
        "an over-budget Audio source must reject before allocation or hashing");

    auto malformed = bytes;
    malformed[0] = 'X';
    writeBytes(validPath.path, malformed);
    snapshot->contentHash =
        talas::project::calculateProjectAssetSha256(malformed);
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                MediaInvalid,
        "a malformed RIFF identity must reject");

    const auto rejectsMedia = [&](auto mutate, std::string_view message) {
        auto changed = bytes;
        mutate(changed);
        writeBytes(validPath.path, changed);
        snapshot->contentHash =
            talas::project::calculateProjectAssetSha256(changed);
        require(
            activation::ComponentActivationAudioSourceFreezerV1::freeze(
                *snapshot)
                    .code
                == activation::ComponentActivationAudioSourceFreezeCodeV1::
                    MediaInvalid,
            std::string(message));
    };
    rejectsMedia([](auto& value) { setU32(value, 4, 0); },
        "RIFF declared-size drift must reject");
    rejectsMedia([](auto& value) { setU32(value, 16, 15); },
        "noncanonical PCM fmt size must reject");
    rejectsMedia([](auto& value) { setU16(value, 20, 3); },
        "non-PCM Audio encoding must reject");
    rejectsMedia([](auto& value) { setU16(value, 22, 3); },
        "more than stereo source channels must reject");
    rejectsMedia(
        [](auto& value) {
            setU32(
                value,
                24,
                activation::maximumComponentActivationAudioSourceSampleRateV1
                    + 1);
        },
        "an over-budget source rate must reject");
    rejectsMedia([](auto& value) { setU32(value, 28, 0); },
        "PCM byte-rate drift must reject");
    rejectsMedia([](auto& value) { setU16(value, 32, 0); },
        "PCM block-alignment drift must reject");
    rejectsMedia([](auto& value) { setU16(value, 34, 24); },
        "non-PCM16 sample width must reject");
    rejectsMedia([](auto& value) { value[36] = 'X'; },
        "a missing data chunk must reject");
    rejectsMedia([](auto& value) { setU32(value, 40, 1); },
        "a non-frame-aligned data chunk must reject");
    rejectsMedia(
        [](auto& value) {
            std::copy_n("fmt ", 4, value.begin() + 36);
        },
        "duplicate fmt chunks must reject");
    rejectsMedia(
        [](auto& value) {
            value.push_back(0);
            setU32(
                value,
                4,
                static_cast<std::uint32_t>(value.size() - 8U));
        },
        "an incomplete trailing chunk header must reject");

    writeBytes(validPath.path, bytes);
    snapshot->contentHash.assign(64, '0');
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                ContentMismatch,
        "a raw Audio digest mismatch must reject");

    snapshot->contentHash = talas::project::calculateProjectAssetSha256(bytes);
    snapshot->channelCount = 1;
    require(
        activation::ComponentActivationAudioSourceFreezerV1::freeze(*snapshot)
                .code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                MediaInvalid,
        "Audio channel metadata drift must reject");

    snapshot->channelCount = 2;
    const auto rejectsSnapshot = [&](auto mutate, std::string_view message) {
        auto changed = *snapshot;
        mutate(changed);
        require(
            activation::ComponentActivationAudioSourceFreezerV1::freeze(changed)
                    .code
                == activation::ComponentActivationAudioSourceFreezeCodeV1::
                    SnapshotInvalid,
            std::string(message));
    };
    rejectsSnapshot([](auto& value) { value.id.clear(); },
        "an empty Audio resource ID must reject");
    rejectsSnapshot([](auto& value) { value.generation = 0; },
        "Audio generation zero must reject");
    rejectsSnapshot([](auto& value) { value.contentHash.front() = 'A'; },
        "a noncanonical Audio digest must reject");
    rejectsSnapshot(
        [](auto& value) { value.kind = talas::core::ResourceSnapshotKind::Binary; },
        "non-Audio snapshot kind must reject");
    rejectsSnapshot([](auto& value) { value.tableSamples.push_back(0.0); },
        "Audio snapshots cannot smuggle Table samples");
    rejectsSnapshot([](auto& value) { value.binaryBytes.push_back(0); },
        "Audio snapshots cannot smuggle Binary bytes");
    rejectsSnapshot([](auto& value) { value.channelCount = 0; },
        "zero Audio snapshot channels must reject");
    rejectsSnapshot([](auto& value) { value.channelCount = 3; },
        "more than stereo snapshot channels must reject");
    rejectsSnapshot([](auto& value) { value.sampleRate = 0.0; },
        "zero Audio snapshot rate must reject");
    rejectsSnapshot([](auto& value) { value.sampleRate = 48'000.5; },
        "fractional Audio snapshot rate must reject");
    rejectsSnapshot([](auto& value) { value.frameCount = 0; },
        "zero Audio snapshot frames must reject");
    rejectsSnapshot([](auto& value) { value.encoding = "float32-wav"; },
        "another Audio snapshot encoding must reject");

    auto changingBytes = pcm16Wav(2, 48'000, 40'000);
    writeBytes(validPath.path, changingBytes);
    snapshot->contentHash =
        talas::project::calculateProjectAssetSha256(changingBytes);
    snapshot->frameCount = 40'000;
    std::uint32_t cancellationChecks = 0;
    const auto changedDuringRead =
        activation::ComponentActivationAudioSourceFreezerV1::freeze(
            *snapshot,
            [&] {
                ++cancellationChecks;
                if (cancellationChecks == 3)
                {
                    std::fstream changed(
                        validPath.path,
                        std::ios::binary | std::ios::in | std::ios::out);
                    changed.seekp(100);
                    changed.put(static_cast<char>(0x7f));
                    changed.flush();
                }
                return false;
            });
    require(
        changedDuringRead.code
            == activation::ComponentActivationAudioSourceFreezeCodeV1::
                SourceChanged,
        "same-path mutation during Audio freezing must reject before publication");
}

void testAudioAuthorityRejectsMissingMismatchedAndSmuggledSources()
{
    AuthorizationFixture missing{AuthorizationFixtureProfile::AudioStream};
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            missing.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::
                AudioSourceInvalid,
        "an Audio profile without an opaque frozen source must reject");

    TemporaryAudioPath path;
    const auto bytes = pcm16Wav();
    writeBytes(path.path, bytes);
    AuthorizationFixture mismatched{
        AuthorizationFixtureProfile::AudioStream};
    auto changed = mismatched.useAudioSource(path.path, bytes);
    ++changed->generation;
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            mismatched.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::
                AudioSourceInvalid,
        "a frozen source cannot be rebound to another snapshot generation");

    AuthorizationFixture exact{AuthorizationFixtureProfile::AudioStream};
    exact.useAudioSource(path.path, bytes);
    AuthorizationFixture stateless;
    stateless.inputs.audioSource = exact.inputs.audioSource;
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            stateless.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::
                ResourceInvalid,
        "a stateless profile cannot smuggle a frozen Audio source");
}

void testAudioAuthoritySharesEveryLiveRevocationBoundary()
{
    const auto makeAudioLease = [](AuthorizationFixture& fixture,
                                   const std::filesystem::path& path,
                                   std::span<const std::uint8_t> bytes) {
        fixture.useAudioSource(path, bytes);
        auto result = activation::ComponentActivationAuthorizationFactory::make(
            fixture.inputs);
        require(result.ok(), "Audio revocation fixture should authorize");
        auto lease = result.authority->consume();
        require(
            lease.has_value() && lease->active(),
            "Audio revocation fixture should issue a lease");
        return lease;
    };
    const auto bytes = pcm16Wav();
    TemporaryAudioPath ownerPath;
    writeBytes(ownerPath.path, bytes);
    AuthorizationFixture ownerFixture{
        AuthorizationFixtureProfile::AudioStream};
    auto ownerLease = makeAudioLease(ownerFixture, ownerPath.path, bytes);
    require(
        ownerFixture.owner.revokeOwnerAction(
                ownerFixture.candidate(), ownerFixture.owner.generation())
            .ok(),
        "Audio owner revocation should apply");
    require(
        !ownerLease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioSourceBytes(*ownerLease)
                .empty(),
        "owner revocation must hide frozen Audio bytes");

    TemporaryAudioPath probePath;
    writeBytes(probePath.path, bytes);
    AuthorizationFixture probeFixture{
        AuthorizationFixtureProfile::AudioStream};
    auto probeLease = makeAudioLease(probeFixture, probePath.path, bytes);
    probeFixture.probeAuthoring.session.revoke();
    require(
        !probeLease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioSourceBytes(*probeLease)
                .empty(),
        "probe revocation must hide frozen Audio bytes");

    TemporaryAudioPath runtimePath;
    writeBytes(runtimePath.path, bytes);
    AuthorizationFixture runtimeFixture{
        AuthorizationFixtureProfile::AudioStream};
    auto runtimeLease = makeAudioLease(
        runtimeFixture, runtimePath.path, bytes);
    runtimeFixture.runtimeAuthoring.session.revoke();
    require(
        !runtimeLease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioSourceBytes(*runtimeLease)
                .empty(),
        "runtime revocation must hide frozen Audio bytes");

    TemporaryAudioPath destroyedPath;
    writeBytes(destroyedPath.path, bytes);
    std::optional<activation::ComponentActivationLeaseV1> destroyedLease;
    {
        auto fixture = std::make_unique<AuthorizationFixture>(
            AuthorizationFixtureProfile::AudioStream);
        destroyedLease = makeAudioLease(*fixture, destroyedPath.path, bytes);
    }
    require(
        destroyedLease.has_value() && !destroyedLease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   audioSourceBytes(*destroyedLease)
                .empty(),
        "authority-source destruction must hide frozen Audio bytes");
}

void testExactImmutableResourceIsDerivedIntoTheOpaqueLease()
{
    AuthorizationFixture fixture{true};
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "one exact immutable table should authorize");
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "resource authority should issue one active lease");
    const auto* grant =
        activation::ComponentActivationAuthorizationTestPeer::resourceGrant(
            *lease);
    const auto bytes =
        activation::ComponentActivationAuthorizationTestPeer::resourceBytes(
            *lease);
    require(
        grant != nullptr
            && grant->resourceGeneration
                == fixture.resourceSnapshotValue->generation
            && std::string_view{
                   grant->resourceId.data(), grant->resourceIdBytes}
                == fixture.resourceSnapshotValue->id
            && bytes.size()
                == fixture.resourceSnapshotValue->tableSamples.size()
                    * sizeof(double)
            && activation::calculateComponentActivationByteDigestV1(bytes)
                == grant->mediatedPayloadDigest
            && activation::validateComponentActivationImmutableResourceGrantV1(
                   *grant,
                   *lease->record(),
                   fixture.sealed,
                   fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the lease should retain the exact derived grant and canonical bytes");
    require(
        std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()}
                .find("/private/project") == std::string_view::npos,
        "the mediated payload must not contain the project path");
}

void testExactImmutableBinaryIsFrozenIntoTheOpaqueLease()
{
    AuthorizationFixture fixture{true};
    const std::vector<std::uint8_t> expected{0x00, 0x7f, 0x80, 0xff};
    auto mutableAlias = fixture.useBinaryResource(expected);
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "one exact immutable Binary should authorize");

    mutableAlias->kind = talas::core::ResourceSnapshotKind::Audio;
    mutableAlias->generation = 99;
    mutableAlias->contentHash.assign(64, '0');
    mutableAlias->binaryBytes.assign(8, 0x55);

    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "Binary resource authority should issue one active lease");
    require(
        !result.authority->consume().has_value(),
        "Binary resource authority must remain one-use");
    const auto* grant =
        activation::ComponentActivationAuthorizationTestPeer::resourceGrant(
            *lease);
    const auto bytes =
        activation::ComponentActivationAuthorizationTestPeer::resourceBytes(
            *lease);
    const auto expectedDigest =
        activation::calculateComponentActivationByteDigestV1(expected);
    require(
        grant != nullptr
            && grant->resourceGeneration == 7
            && grant->resourceKind
                == static_cast<std::uint32_t>(
                    activation::ComponentActivationImmutableResourceKindV1::
                        Binary)
            && grant->elementFormat
                == static_cast<std::uint32_t>(
                    activation::
                        ComponentActivationImmutableResourceElementFormatV1::
                            RawBytes)
            && grant->elementCount == expected.size()
            && grant->byteCount == expected.size()
            && grant->sourceContentDigest == expectedDigest
            && grant->mediatedPayloadDigest == expectedDigest
            && std::ranges::equal(bytes, expected)
            && activation::validateComponentActivationImmutableResourceGrantV1(
                   *grant,
                   *lease->record(),
                   fixture.sealed,
                   fixture.staticParameters)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the lease must freeze exact Binary identity, counts, digest, and bytes");
    require(
        std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()}
                .find("/private/project") == std::string_view::npos,
        "the Binary authority must never serialize its resolved project path");

    lease->revoke();
    require(
        !lease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceGrant(*lease)
                == nullptr
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceBytes(*lease)
                .empty(),
        "Binary bytes and grant must disappear with existing lease revocation");
}

void testBinaryAuthorityRejectsEverySnapshotContradiction()
{
    const auto rejects = [](auto mutate, std::string_view message) {
        AuthorizationFixture fixture{true};
        auto changed = fixture.useBinaryResource({0x00, 0x7f, 0x80, 0xff});
        mutate(*changed);
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            !result.ok()
                && result.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        ResourceInvalid,
            std::string(message));
    };
    rejects([](auto& value) { value.binaryBytes.clear(); },
        "an empty Binary authority payload must reject");
    rejects([](auto& value) { value.id = "Tables/Other"; },
        "a Binary snapshot for another resource ID must reject");
    rejects([](auto& value) { value.generation = 0; },
        "a legacy Binary generation must reject");
    rejects([](auto& value) {
        value.binaryBytes.assign(
            activation::maximumComponentActivationImmutableBinaryBytesV1
                + 1U,
            0x33);
    }, "an over-budget Binary authority payload must reject");
    rejects([](auto& value) { value.contentHash.assign(64, '0'); },
        "a Binary source digest mismatch must reject");
    rejects([](auto& value) { value.binaryBytes.front() ^= 0xffU; },
        "Binary byte mutation without matching source identity must reject");
    rejects([](auto& value) { value.contentHash.front() = 'A'; },
        "a noncanonical Binary source digest must reject");
    rejects([](auto& value) { value.tableSamples.push_back(1.0); },
        "Binary authority cannot smuggle Table samples");
    rejects([](auto& value) { value.channelCount = 1; },
        "Binary authority cannot smuggle audio channels");
    rejects([](auto& value) { value.sampleRate = 48'000.0; },
        "Binary authority cannot smuggle an audio sample rate");
    rejects([](auto& value) { value.frameCount = 1; },
        "Binary authority cannot smuggle an audio frame count");
    rejects([](auto& value) { value.encoding = "opaque"; },
        "Binary authority cannot introduce host-side decoding semantics");
    rejects([](auto& value) { value.kind = talas::core::ResourceSnapshotKind::Audio; },
        "an unsupported resource kind must reject");

    AuthorizationFixture maximum{true};
    maximum.useBinaryResource(std::vector<std::uint8_t>(
        activation::maximumComponentActivationImmutableBinaryBytesV1,
        0x5a));
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            maximum.inputs)
            .ok(),
        "the exact one-MiB Binary authority payload should authorize");
}

void testBinaryAuthoritySharesDenialAndEveryLiveRevocationBoundary()
{
    {
        AuthorizationFixture denied{true};
        denied.useBinaryResource({0x00, 0x80, 0xff});
        require(
            denied.owner.replaceOwnerAction(
                    denied.candidate(),
                    bundle::ComponentOwnerAuthorizationAction::Deny,
                    denied.owner.generation())
                .ok(),
            "Binary denial fixture should replace the prior approval");
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                denied.inputs);
        require(
            !result.ok()
                && result.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        OwnerNotApproved,
            "an exact Binary owner denial must reject without exposing bytes");
    }
    {
        AuthorizationFixture fixture{true};
        fixture.useBinaryResource({0x00, 0x80, 0xff});
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(result.ok(), "Binary owner-revoke fixture should authorize");
        auto lease = result.authority->consume();
        require(lease.has_value() && lease->active(),
            "Binary owner-revoke fixture should issue a lease");
        require(
            fixture.owner.revokeOwnerAction(
                    fixture.candidate(), fixture.owner.generation())
                .ok(),
            "Binary owner revocation should apply");
        require(
            !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceGrant(*lease)
                    == nullptr
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceBytes(*lease)
                    .empty(),
            "owner revocation must hide the Binary grant and bytes");
    }
    {
        AuthorizationFixture fixture{true};
        fixture.useBinaryResource({0x00, 0x80, 0xff});
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(result.ok(), "Binary probe-revoke fixture should authorize");
        auto lease = result.authority->consume();
        fixture.probeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceBytes(*lease)
                    .empty(),
            "probe revocation must hide retained Binary bytes");
    }
    {
        AuthorizationFixture fixture{true};
        fixture.useBinaryResource({0x00, 0x80, 0xff});
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(result.ok(), "Binary runtime-revoke fixture should authorize");
        auto lease = result.authority->consume();
        fixture.runtimeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceBytes(*lease)
                    .empty(),
            "runtime revocation must hide retained Binary bytes");
    }
    {
        std::optional<activation::ComponentActivationLeaseV1> lease;
        {
            auto fixture = std::make_unique<AuthorizationFixture>(true);
            fixture->useBinaryResource({0x00, 0x80, 0xff});
            auto result =
                activation::ComponentActivationAuthorizationFactory::make(
                    fixture->inputs);
            require(result.ok(),
                "Binary store-destruction fixture should authorize");
            lease = result.authority->consume();
            require(
                lease.has_value() && lease->active(),
                "Binary store-destruction fixture should issue a lease");
        }
        require(
            !lease->active()
                && activation::ComponentActivationAuthorizationTestPeer::
                       resourceBytes(*lease)
                    .empty(),
            "authority-source destruction must hide retained Binary bytes");
    }
}

void testResourceAuthorityRejectsEverySnapshotWidening()
{
    const auto rejects = [](
                             auto mutate,
                             std::string_view message) {
        AuthorizationFixture fixture{true};
        auto changed = std::make_shared<talas::core::ResourceSnapshot>(
            *fixture.resourceSnapshotValue);
        mutate(*changed);
        fixture.resourceSnapshotValue = changed;
        fixture.inputs.resourceSnapshot = changed;
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            !result.ok()
                && result.code
                    == activation::ComponentActivationAuthorizationCodeV1::
                        ResourceInvalid,
            std::string(message));
    };
    rejects([](auto& value) { value.id = "Tables/Other"; },
        "another resource ID must reject");
    rejects([](auto& value) {
        value.kind = talas::core::ResourceSnapshotKind::Binary;
    }, "kind drift without a matching Binary payload must reject");
    rejects([](auto& value) { value.generation = 0; },
        "legacy generation zero must reject");
    rejects([](auto& value) { value.contentHash = std::string(63, '6'); },
        "short source hashes must reject");
    rejects([](auto& value) { value.contentHash.front() = 'A'; },
        "noncanonical source hashes must reject");
    rejects([](auto& value) { value.tableSamples.clear(); },
        "empty tables must reject");
    rejects([](auto& value) {
        value.tableSamples.front() =
            std::numeric_limits<double>::quiet_NaN();
    }, "non-finite tables must reject");
    rejects([](auto& value) { value.binaryBytes.push_back(1); },
        "contradictory binary payload claims must reject");
    rejects([](auto& value) { value.channelCount = 1; },
        "contradictory audio metadata must reject");
    rejects([](auto& value) {
        value.tableSamples.resize(
            activation::maximumComponentActivationImmutableTableElementsV1
            + 1,
            0.0);
    }, "over-budget tables must reject");

    AuthorizationFixture missing{true};
    missing.inputs.resourceSnapshot.reset();
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            missing.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::
                ResourceInvalid,
        "a missing resource snapshot must reject");

    AuthorizationFixture smuggled;
    smuggled.inputs.resourceSnapshot =
        AuthorizationFixture{true}.resourceSnapshotValue;
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            smuggled.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::
                ResourceInvalid,
        "the stateless profile must reject an unbound resource snapshot");
}

void testResourceLeaseFreezesBytesAndSharesExistingRevocation()
{
    AuthorizationFixture fixture{true};
    auto mutableSnapshot = std::make_shared<talas::core::ResourceSnapshot>(
        *fixture.resourceSnapshotValue);
    fixture.inputs.resourceSnapshot = mutableSnapshot;
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "mutable-alias fixture should authorize exact input");

    mutableSnapshot->id = "Tables/Mutated";
    mutableSnapshot->generation = 99;
    mutableSnapshot->tableSamples.assign(3, 99.0);
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "post-authoring input mutation must not alter the issued authority");
    const auto* grant =
        activation::ComponentActivationAuthorizationTestPeer::resourceGrant(
            *lease);
    const auto bytes =
        activation::ComponentActivationAuthorizationTestPeer::resourceBytes(
            *lease);
    require(
        grant != nullptr && grant->resourceGeneration == 7
            && std::string_view{
                   grant->resourceId.data(), grant->resourceIdBytes}
                == "Tables/Curve-A"
            && bytes.size() == 3 * sizeof(double),
        "the authority must own frozen identity and canonical bytes, not a caller alias");

    fixture.probeAuthoring.session.revoke();
    require(
        !lease->active()
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceGrant(*lease)
                == nullptr
            && activation::ComponentActivationAuthorizationTestPeer::
                   resourceBytes(*lease)
                .empty(),
        "resource grant and bytes must disappear with existing lease revocation");
}

void testAuthorityIsOpaqueMoveOnlySingleUseAndExactlyBound()
{
    static_assert(!std::is_aggregate_v<activation::ComponentActivationAuthorityV1>);
    static_assert(!std::is_copy_constructible_v<
        activation::ComponentActivationAuthorityV1>);
    static_assert(!std::is_copy_constructible_v<
        activation::ComponentActivationLeaseV1>);
    static_assert(!std::is_copy_constructible_v<
        activation::ComponentActivationRuntimeSessionV1>);
    static_assert(!std::is_copy_constructible_v<
        activation::ComponentActivationFrozenAudioSourceV1>);
    static_assert(!std::is_aggregate_v<
        activation::ComponentActivationFrozenAudioSourceV1>);

    AuthorizationFixture fixture;
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "complete exact live evidence should authorize");
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active() && lease->record() != nullptr,
        "the authority should produce one active consumed lease");
    require(
        !result.authority->consume().has_value(),
        "the same authority must reject replay after one consume");
    const auto* record = lease->record();
    require(
        record != nullptr
            && record->activationGeneration == fixture.inputs.activationGeneration
            && record->contentDigest == fixture.snapshotValue.contentDigest
            && activation::validateComponentActivationAuthorizationV1(
                   *record, fixture.sealed)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the consumed lease should expose only its exact validated record");
}

void testEveryLiveSourceRevokesAnIssuedLease()
{
    {
        AuthorizationFixture fixture;
        auto result = activation::ComponentActivationAuthorizationFactory::make(
            fixture.inputs);
        require(result.ok(), "owner revoke fixture should authorize");
        auto lease = result.authority->consume();
        require(
            lease.has_value() && lease->active(),
            "owner revoke fixture should issue an active lease");
        ComponentApprovalFingerprint candidate;
        const auto* record = lease->record();
        candidate.bundleId = {record->bundleId.data(), record->bundleIdBytes};
        candidate.semanticVersion = {
            record->semanticVersion.data(), record->semanticVersionBytes};
        candidate.contentDigest = record->contentDigest;
        candidate.signerIdentityDigest = record->signerIdentityDigest;
        candidate.requestedPermissions = record->requestedPermissions;
        candidate.policyVersion = record->policyVersion;
        candidate.hasContentDigest = true;
        candidate.hasSignerIdentity = true;
        require(
            fixture.owner.revokeOwnerAction(
                    candidate, fixture.owner.generation())
                    .ok()
                && !lease->active() && lease->record() == nullptr,
            "owner revocation must silence an issued lease");
    }
    {
        AuthorizationFixture fixture;
        auto result = activation::ComponentActivationAuthorizationFactory::make(
            fixture.inputs);
        require(result.ok(), "probe revoke fixture should authorize");
        auto lease = result.authority->consume();
        require(
            lease.has_value() && lease->active(),
            "probe revoke fixture should issue an active lease");
        fixture.probeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active(),
            "probe revocation must silence an issued lease");
    }
    {
        AuthorizationFixture fixture;
        auto result = activation::ComponentActivationAuthorizationFactory::make(
            fixture.inputs);
        require(result.ok(), "runtime revoke fixture should authorize");
        auto lease = result.authority->consume();
        require(
            lease.has_value() && lease->active(),
            "runtime revoke fixture should issue an active lease");
        fixture.runtimeAuthoring.session.revoke();
        require(
            lease.has_value() && !lease->active(),
            "runtime-session revocation must silence an issued lease");
    }
}

void testCachedAuthorizationRecordRemainsSafeAcrossRevocation()
{
    const auto exercise = [](AuthorizationFixture& fixture, auto revoke) {
        auto result =
            activation::ComponentActivationAuthorizationFactory::make(
                fixture.inputs);
        require(
            result.ok(), "authorization snapshot fixture should authorize");
        auto lease = result.authority->consume();
        require(
            lease.has_value() && lease->record() != nullptr,
            "authorization snapshot fixture should expose one live record");
        const auto* cached = lease->record();
        const auto expected = *cached;
        revoke();
        require(
            lease->record() == nullptr
                && cached->activationGeneration
                    == expected.activationGeneration
                && cached->authorizationNonce == expected.authorizationNonce
                && cached->contentDigest == expected.contentDigest,
            "a one-time cached immutable record must remain readable while live revalidation fails closed");
    };
    {
        AuthorizationFixture fixture;
        exercise(fixture, [&] {
            require(
                fixture.owner
                    .revokeOwnerAction(
                        fixture.candidate(), fixture.owner.generation())
                    .ok(),
                "owner snapshot revocation should apply");
        });
    }
    {
        AuthorizationFixture fixture;
        exercise(fixture, [&] { fixture.probeAuthoring.session.revoke(); });
    }
    {
        AuthorizationFixture fixture;
        exercise(fixture, [&] { fixture.runtimeAuthoring.session.revoke(); });
    }
}

void testStageLeaseAdmissionAllocatesNoObjects()
{
    AuthorizationFixture fixture;
    auto result = activation::ComponentActivationAuthorizationFactory::make(fixture.inputs);
    require(result.ok(), "allocation-free admission fixture must authorize");
    auto lease = result.authority->consume();
    require(lease && lease->active(), "allocation-free admission must own live authority");
    failNextAllocation.store(true, std::memory_order_relaxed);
    auto stage = lease->tryAcquireStageLease();
    const auto unusedFault = failNextAllocation.exchange(false, std::memory_order_relaxed);
    require(unusedFault && stage && stage->active(),
        "stage admission must remain live without allocating a C++ object");

    AuthorizationFixture second;
    auto secondResult = activation::ComponentActivationAuthorizationFactory::make(second.inputs);
    require(secondResult.ok(), "second move-assignment authority must authorize");
    auto secondLease = secondResult.authority->consume();
    require(secondLease && secondLease->active(), "second move-assignment lease must be live");
    auto secondStage = secondLease->tryAcquireStageLease();
    require(secondStage && secondStage->active(), "second stage must own independent admission");
    failNextAllocation.store(true, std::memory_order_relaxed);
    auto moved = std::move(*secondStage);
    *stage = std::move(moved);
    const auto unusedMoveFault = failNextAllocation.exchange(false, std::memory_order_relaxed);
    require(unusedMoveFault && stage->active() && !moved.active() && !secondStage->active(),
        "stage moves transfer locks and authority without allocation or live moved-from handles");
    lease->revoke();
    require(!lease->active() && stage->active(), "move assignment releases the previous authority gates");
    *stage = activation::ComponentActivationStageLeaseV1{};
    require(!stage->active(), "empty stage assignment releases the replacement admission");
    secondLease->revoke();
    require(!secondLease->active(), "replacement authority can revoke after moved stage release");
}

void testStageLeaseLinearizesOwnerRevocation()
{
    AuthorizationFixture fixture;
    auto result = activation::ComponentActivationAuthorizationFactory::make(
        fixture.inputs);
    require(result.ok(), "stage admission fixture should authorize");
    auto lease = result.authority->consume();
    require(
        lease.has_value() && lease->active(),
        "stage admission fixture should issue a live lease");
    auto stageLease = lease->tryAcquireStageLease();
    require(
        stageLease.has_value() && stageLease->active(),
        "a live authority should acquire one stage admission lease");

    const auto* record = lease->record();
    ComponentApprovalFingerprint candidate;
    candidate.bundleId = {record->bundleId.data(), record->bundleIdBytes};
    candidate.semanticVersion = {
        record->semanticVersion.data(), record->semanticVersionBytes};
    candidate.contentDigest = record->contentDigest;
    candidate.signerIdentityDigest = record->signerIdentityDigest;
    candidate.requestedPermissions = record->requestedPermissions;
    candidate.policyVersion = record->policyVersion;
    candidate.hasContentDigest = true;
    candidate.hasSignerIdentity = true;

    std::atomic<bool> revokeEntered{false};
    std::atomic<bool> revokeCompleted{false};
    std::thread revokeThread([&] {
        revokeEntered.store(true, std::memory_order_release);
        const auto revoked = fixture.owner.revokeOwnerAction(
            candidate,
            fixture.owner.generation());
        revokeCompleted.store(revoked.ok(), std::memory_order_release);
    });
    while (!revokeEntered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(
        !revokeCompleted.load(std::memory_order_acquire),
        "owner revocation must not complete across an admitted graph publication");
    stageLease.reset();
    revokeThread.join();
    require(
        revokeCompleted.load(std::memory_order_acquire) && !lease->active(),
        "owner revocation must complete immediately after graph publication releases admission");
}

void testFabricatedStaleAndUnsupportedInputsFailClosed()
{
    AuthorizationFixture fixture;
    const auto rejects = [&](auto mutate,
                             activation::ComponentActivationAuthorizationCodeV1 code,
                             std::string_view message) {
        auto changed = fixture.inputs;
        mutate(changed);
        const auto result =
            activation::ComponentActivationAuthorizationFactory::make(changed);
        require(
            !result.ok() && result.code == code,
            std::string(message));
    };
    rejects(
        [](auto& value) { value.signature = {}; },
        activation::ComponentActivationAuthorizationCodeV1::SignatureInvalid,
        "caller-written missing signature must reject");
    rejects(
        [](auto& value) { value.ownerAuthorization = {}; },
        activation::ComponentActivationAuthorizationCodeV1::OwnerNotApproved,
        "caller-written missing owner evidence must reject");
    rejects(
        [](auto& value) { value.probe = {}; },
        activation::ComponentActivationAuthorizationCodeV1::ProbeInvalid,
        "caller-written missing probe evidence must reject");
    rejects(
        [](auto& value) { value.runtime = {}; },
        activation::ComponentActivationAuthorizationCodeV1::RuntimeInvalid,
        "caller-written runtime evidence must reject");
    rejects(
        [](auto& value) { ++value.artifactGeneration; },
        activation::ComponentActivationAuthorizationCodeV1::ProbeInvalid,
        "stale artifact generation must reject");
    rejects(
        [](auto& value) { value.validationNonce.front() ^= 0xffU; },
        activation::ComponentActivationAuthorizationCodeV1::ProbeInvalid,
        "stale validation session must reject");
    rejects(
        [](auto& value) { value.activationGeneration = 0; },
        activation::ComponentActivationAuthorizationCodeV1::SessionBindingInvalid,
        "missing activation generation must reject");
    rejects(
        [](auto& value) { value.authorizationNonce.fill(0); },
        activation::ComponentActivationAuthorizationCodeV1::SessionBindingInvalid,
        "missing authorization nonce must reject");
    rejects(
        [](auto& value) { value.sampleRate = 96'000; },
        activation::ComponentActivationAuthorizationCodeV1::ProfileUnsupported,
        "unsupported sample rate must reject");
    rejects(
        [](auto& value) { value.opaqueStateByteCount = 1; },
        activation::ComponentActivationAuthorizationCodeV1::ProfileUnsupported,
        "non-empty state must reject");
    rejects(
        [](auto& value) { value.descriptorId = "org.talas.activation.other"; },
        activation::ComponentActivationAuthorizationCodeV1::DescriptorInvalid,
        "another descriptor must reject");
    rejects(
        [](auto& value) { value.staticParameters = {}; },
        activation::ComponentActivationAuthorizationCodeV1::ProfileUnsupported,
        "missing static parameter values must reject");

    auto changedParameters = fixture.staticParameters;
    changedParameters.front().kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Integer);
    auto changed = fixture.inputs;
    changed.staticParameters = changedParameters;
    require(
        activation::ComponentActivationAuthorizationFactory::make(changed).code
            == activation::ComponentActivationAuthorizationCodeV1::ProfileUnsupported,
        "a static parameter kind that differs from the manifest must reject");

    changedParameters = fixture.staticParameters;
    changedParameters.front().id.front() = 'x';
    changed.staticParameters = changedParameters;
    require(
        activation::ComponentActivationAuthorizationFactory::make(changed).code
            == activation::ComponentActivationAuthorizationCodeV1::ProfileUnsupported,
        "a static parameter id that differs from the manifest must reject");
}

void testRevokedEvidenceCannotCreateAnAuthority()
{
    AuthorizationFixture fixture;
    fixture.probeAuthoring.session.revoke();
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            fixture.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::ProbeInvalid,
        "a revoked probe must reject before authority creation");

    AuthorizationFixture runtimeFixture;
    runtimeFixture.runtimeAuthoring.session.revoke();
    require(
        activation::ComponentActivationAuthorizationFactory::make(
            runtimeFixture.inputs)
                .code
            == activation::ComponentActivationAuthorizationCodeV1::RuntimeInvalid,
        "a revoked runtime session must reject before authority creation");
}
}

int main()
{
    testAtomicMultipleResourceAuthorityOwnsOneMixedSet();
    testMultipleResourceAuthorityRejectsEveryPartialSet();
    testMultipleResourceLeaseRevokesTheCompleteSetAtomically();
    testMultipleResourceSetSharesEveryLiveBoundary();
    testMultipleResourceStageLeaseLinearizesRevocation();
    testMultipleResourceAudioAndAggregateBoundaries();
    testMultipleResourceMemberCountBoundary();
    testAudioSourceFreezerAndAuthorityOwnExactPathlessBytes();
    testAudioSourceFreezerRejectsPathMediaAndMutationFailures();
    testAudioAuthorityRejectsMissingMismatchedAndSmuggledSources();
    testAudioAuthoritySharesEveryLiveRevocationBoundary();
    testExactImmutableResourceIsDerivedIntoTheOpaqueLease();
    testExactImmutableBinaryIsFrozenIntoTheOpaqueLease();
    testResourceAuthorityRejectsEverySnapshotWidening();
    testBinaryAuthorityRejectsEverySnapshotContradiction();
    testBinaryAuthoritySharesDenialAndEveryLiveRevocationBoundary();
    testResourceLeaseFreezesBytesAndSharesExistingRevocation();
    testAuthorityIsOpaqueMoveOnlySingleUseAndExactlyBound();
    testEveryLiveSourceRevokesAnIssuedLease();
    testCachedAuthorizationRecordRemainsSafeAcrossRevocation();
    testStageLeaseAdmissionAllocatesNoObjects();
    testStageLeaseLinearizesOwnerRevocation();
    testFabricatedStaleAndUnsupportedInputsFailClosed();
    testRevokedEvidenceCannotCreateAnAuthority();
    return 0;
}
