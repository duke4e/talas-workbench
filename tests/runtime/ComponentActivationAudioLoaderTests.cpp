#include "runtime/component_activation/macos/ComponentActivationLoader.h"
#include "runtime/component_activation/ComponentActivationWorker.h"
#include "runtime/component_bundle/ComponentBundleInspection.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <new>
#include <limits>

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

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

std::vector<std::uint8_t> sourceWave()
{
    constexpr std::uint32_t frames = 2'048U;
    constexpr std::uint32_t dataBytes = frames * 2U;
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    appendU32(bytes, 36U + dataBytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    appendU32(bytes, 16U);
    appendU16(bytes, 1U);
    appendU16(bytes, 1U);
    appendU32(bytes, 48'000U);
    appendU32(bytes, 96'000U);
    appendU16(bytes, 2U);
    appendU16(bytes, 16U);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(bytes, dataBytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        appendU16(bytes, static_cast<std::uint16_t>(frame));
    return bytes;
}

struct AudioFixture
{
    activation::ComponentActivationSealedSessionV1 session;
    activation::ComponentActivationAuthorizationV1 authorization;
    std::array<activation::ComponentActivationStaticParameterV1, 1> parameters;
    activation::ComponentActivationAudioStreamGrantV1 grant;
    std::vector<std::uint8_t> sourceBytes = sourceWave();
    bundle::ComponentBundleSnapshot snapshot;
    std::vector<std::uint8_t> entryBytes;

    explicit AudioFixture(const char* fixturePath)
    {
        std::ifstream stream(fixturePath, std::ios::binary);
        require(stream.good(), "Audio loader fixture should open");
        entryBytes.assign(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
        require(!entryBytes.empty(), "Audio loader fixture should have bytes");
        const auto entryDigest =
            activation::calculateComponentActivationByteDigestV1(entryBytes);

        std::ostringstream json;
        json << R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.talas.activation.fixture",
  "semanticVersion":"1.0.0",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":3,"apiMaximum":3},
  "roles":["bounded-audio-stream-consumer","realtime-processor"],
  "permissions":["bounded-audio-stream-read","realtime-audio"],
  "entry":{"path":"Payload/audio.bundle","bytes":)json"
             << entryBytes.size() << R"json(,"sha256":")json"
             << digestHex(entryDigest) << R"json("},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.fixture.gain",
    "stateSchemaVersion":1,
    "inputs":[{"id":"in","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "outputs":[{"id":"out","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "parameters":[{"id":"audio","kind":"resource","domain":"scalar","choices":[]}]
  }],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Talas tests","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
        const auto parsed = bundle::parseComponentBundleManifest(json.str());
        require(parsed.ok(), "Audio loader manifest should parse");
        const auto canonical =
            bundle::writeCanonicalComponentBundleManifest(*parsed.manifest);
        require(canonical.ok(), "Audio loader manifest should canonicalize");
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
            "Audio loader snapshot should be exact");

        session.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::SealedSession,
            sizeof(session));
        session.profileVersion = activation::componentActivationProfileVersionV1;
        session.supportedPermissions = componentPermission(
                ComponentExecutionPermission::RealtimeAudio)
            | componentPermission(
                ComponentExecutionPermission::BoundedAudioStreamRead);
        session.safeguards = allComponentIsolationSafeguardsV1;
        session.runtimeSessionGeneration = 17U;
        session.sharedStorageIdentity = 23U;
        session.sharedStorageBytes = 65'536U;
        session.runtimeSessionNonce.fill(0x33U);

        auto& parameter = parameters.front();
        parameter.kind = static_cast<std::uint32_t>(
            activation::ComponentActivationStaticParameterKindV1::Resource);
        constexpr std::string_view parameterId = "audio";
        constexpr std::string_view resourceId = "Audio/Voice-A";
        parameter.idBytes = parameterId.size();
        parameter.textBytes = resourceId.size();
        std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
        std::copy(resourceId.begin(), resourceId.end(), parameter.text.begin());

        authorization.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::Authorization,
            sizeof(authorization));
        authorization.bundleIdBytes = snapshot.manifest.bundleId.size();
        authorization.semanticVersionBytes =
            snapshot.manifest.semanticVersion.size();
        constexpr std::string_view descriptorId =
            "org.talas.activation.fixture.gain";
        authorization.descriptorIdBytes = descriptorId.size();
        std::copy(
            snapshot.manifest.bundleId.begin(),
            snapshot.manifest.bundleId.end(),
            authorization.bundleId.begin());
        std::copy(
            snapshot.manifest.semanticVersion.begin(),
            snapshot.manifest.semanticVersion.end(),
            authorization.semanticVersion.begin());
        std::copy(
            descriptorId.begin(), descriptorId.end(),
            authorization.descriptorId.begin());
        authorization.contentDigest = snapshot.contentDigest;
        authorization.signerIdentityDigest.fill(0x42U);
        authorization.entryDigest = entryDigest;
        authorization.requestedPermissions =
            snapshot.manifest.requestedPermissions;
        authorization.declaredRoles = snapshot.manifest.declaredRoles;
        authorization.policyVersion = currentComponentTrustPolicyVersion;
        authorization.sampleRate = 48'000U;
        authorization.blockFrames = 256U;
        authorization.inputChannelCount = 1U;
        authorization.outputChannelCount = 1U;
        authorization.staticParameterCount = parameters.size();
        authorization.staticParameterDigest =
            activation::calculateComponentActivationStaticParameterDigestV1(
                parameters);
        authorization.entryByteCount = entryBytes.size();
        authorization.artifactGeneration = 29U;
        authorization.activationGeneration = 31U;
        authorization.runtimeSessionGeneration = session.runtimeSessionGeneration;
        authorization.sharedStorageIdentity = session.sharedStorageIdentity;
        authorization.validationNonce.fill(0x5cU);
        authorization.runtimeSessionNonce = session.runtimeSessionNonce;
        authorization.authorizationNonce.fill(0xa5U);

        grant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::AudioStreamGrant,
            sizeof(grant));
        grant.contentDigest = authorization.contentDigest;
        grant.signerIdentityDigest = authorization.signerIdentityDigest;
        grant.sourceContentDigest =
            activation::calculateComponentActivationByteDigestV1(sourceBytes);
        grant.validationNonce = authorization.validationNonce;
        grant.runtimeSessionNonce = authorization.runtimeSessionNonce;
        grant.authorizationNonce = authorization.authorizationNonce;
        grant.artifactGeneration = authorization.artifactGeneration;
        grant.activationGeneration = authorization.activationGeneration;
        grant.runtimeSessionGeneration = authorization.runtimeSessionGeneration;
        grant.sharedStorageIdentity = authorization.sharedStorageIdentity;
        grant.resourceGeneration = 7U;
        grant.sourceByteCount = sourceBytes.size();
        grant.sourceFrameCount = 2'048U;
        grant.maximumSourceFrameIncrement = 64.0;
        grant.parameterIdBytes = parameterId.size();
        grant.resourceIdBytes = resourceId.size();
        grant.sourceSampleRate = 48'000U;
        grant.streamProfileVersion =
            activation::componentActivationAudioStreamProfileVersionV1;
        grant.sourceEncoding = static_cast<std::uint32_t>(
            activation::ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
        grant.maximumReadFrames =
            activation::maximumComponentActivationAudioReadFramesV1;
        grant.decodeBlockFrames =
            activation::componentActivationAudioDecodeBlockFramesV1;
        grant.sourceChannelCount = 1U;
        grant.maximumTouchedSourceBlocks =
            activation::maximumComponentActivationAudioTouchedSourceBlocksV1;
        grant.requestCapacity =
            activation::componentActivationAudioRequestCapacityV1;
        grant.responseCapacity =
            activation::componentActivationAudioResponseCapacityV1;
        grant.workerCacheCapacity =
            activation::componentActivationAudioWorkerCacheCapacityV1;
        grant.lowWatermarkBlocks =
            activation::componentActivationAudioLowWatermarkBlocksV1;
        grant.highWatermarkBlocks =
            activation::componentActivationAudioHighWatermarkBlocksV1;
        std::copy(parameterId.begin(), parameterId.end(), grant.parameterId.begin());
        std::copy(resourceId.begin(), resourceId.end(), grant.resourceId.begin());
    }
};

template <typename Record>
std::array<std::uint8_t, componentTrustDigestBytes> recordDigest(
    const Record& record)
{
    return activation::calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&record),
            sizeof(record)});
}

std::uint64_t roundedResourceSetBytes(std::uint64_t bytes)
{
    constexpr auto page =
        activation::maximumComponentActivationResourceSetTransportPageBytesV1;
    return ((bytes + page - 1U) / page) * page;
}

struct ResourceSetLoaderFixture
{
    activation::ComponentActivationSealedSessionV1 session;
    activation::ComponentActivationAuthorizationV1 authorization;
    std::array<activation::ComponentActivationStaticParameterV1, 4>
        parameters{};
    activation::ComponentActivationImmutableResourceGrantV1 tableGrant;
    activation::ComponentActivationImmutableResourceGrantV1 binaryGrant;
    std::array<activation::ComponentActivationAudioStreamGrantV1, 2>
        audioGrants{};
    activation::ComponentActivationResourceSetGrantV1 setGrant;
    std::array<activation::ComponentActivationResourceSetMemberV1, 4>
        members{};
    std::array<activation::ComponentActivationPreparedResourceSetMemberV1, 4>
        preparedMembers{};
    std::array<std::uint8_t, 3U * sizeof(double)> tableBytes{};
    std::array<std::uint8_t, 5> binaryBytes{0x00, 0x7f, 0x80, 0xfe, 0xff};
    std::array<std::vector<std::uint8_t>, 2> audioBytes{
        sourceWave(), sourceWave()};
    bundle::ComponentBundleSnapshot snapshot;
    std::vector<std::uint8_t> entryBytes;

    explicit ResourceSetLoaderFixture(const char* fixturePath)
    {
        std::ifstream stream(fixturePath, std::ios::binary);
        require(stream.good(), "resource-set loader fixture should open");
        entryBytes.assign(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
        require(
            !entryBytes.empty(),
            "resource-set loader fixture should have bytes");
        const auto entryDigest =
            activation::calculateComponentActivationByteDigestV1(entryBytes);

        std::ostringstream json;
        json << R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.talas.activation.fixture",
  "semanticVersion":"1.0.0",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":4,"apiMaximum":4},
  "roles":["bounded-audio-stream-consumer","immutable-resource-consumer","realtime-processor"],
  "permissions":["bounded-audio-stream-read","immutable-resource-read","realtime-audio"],
  "entry":{"path":"Payload/resource-set.bundle","bytes":)json"
             << entryBytes.size() << R"json(,"sha256":")json"
             << digestHex(entryDigest) << R"json("},
  "files":[],
  "nodes":[{
    "typeId":"org.talas.activation.fixture.gain",
    "stateSchemaVersion":1,
    "inputs":[{"id":"in","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "outputs":[{"id":"out","type":"audio","required":true,"rate":"sample","audioLayout":"mono","edge":"continuous","spectralFormat":"not-applicable","timelineBearing":true}],
    "parameters":[
      {"id":"table","kind":"resource","domain":"scalar","choices":[]},
      {"id":"binary","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-a","kind":"resource","domain":"scalar","choices":[]},
      {"id":"audio-b","kind":"resource","domain":"scalar","choices":[]}
    ]
  }],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Talas tests","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
        const auto parsed = bundle::parseComponentBundleManifest(json.str());
        if (!parsed.ok())
        {
            for (const auto& error : parsed.errors)
            {
                std::cerr << "resource-set manifest error " << error.path
                          << ": " << error.summary << '\n';
            }
        }
        require(parsed.ok(), "resource-set loader manifest should parse");
        const auto canonical =
            bundle::writeCanonicalComponentBundleManifest(*parsed.manifest);
        require(
            canonical.ok(),
            "resource-set loader manifest should canonicalize");
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
            "resource-set loader snapshot should be exact");

        session.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::SealedSession,
            sizeof(session));
        session.profileVersion =
            activation::componentActivationProfileVersionV1;
        session.supportedPermissions =
            componentPermission(ComponentExecutionPermission::RealtimeAudio)
            | componentPermission(
                ComponentExecutionPermission::ImmutableResourceRead)
            | componentPermission(
                ComponentExecutionPermission::BoundedAudioStreamRead);
        session.safeguards = allComponentIsolationSafeguardsV1;
        session.runtimeSessionGeneration = 17U;
        session.sharedStorageIdentity = 23U;
        session.sharedStorageBytes = 65'536U;
        session.runtimeSessionNonce.fill(0x33U);

        constexpr std::array<std::string_view, 4> parameterIds{
            "table", "binary", "audio-a", "audio-b"};
        constexpr std::array<std::string_view, 4> resourceIds{
            "Tables/Curve-A", "Binary/Blob-A", "Audio/Voice-A", "Audio/Voice-B"};
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

        authorization.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::Authorization,
            sizeof(authorization));
        authorization.bundleIdBytes = snapshot.manifest.bundleId.size();
        authorization.semanticVersionBytes =
            snapshot.manifest.semanticVersion.size();
        constexpr std::string_view descriptorId =
            "org.talas.activation.fixture.gain";
        authorization.descriptorIdBytes = descriptorId.size();
        std::copy(
            snapshot.manifest.bundleId.begin(),
            snapshot.manifest.bundleId.end(),
            authorization.bundleId.begin());
        std::copy(
            snapshot.manifest.semanticVersion.begin(),
            snapshot.manifest.semanticVersion.end(),
            authorization.semanticVersion.begin());
        std::copy(
            descriptorId.begin(),
            descriptorId.end(),
            authorization.descriptorId.begin());
        authorization.contentDigest = snapshot.contentDigest;
        authorization.signerIdentityDigest.fill(0x42U);
        authorization.entryDigest = entryDigest;
        authorization.requestedPermissions =
            snapshot.manifest.requestedPermissions;
        authorization.declaredRoles = snapshot.manifest.declaredRoles;
        authorization.policyVersion = currentComponentTrustPolicyVersion;
        authorization.sampleRate = 48'000U;
        authorization.blockFrames = 256U;
        authorization.inputChannelCount = 1U;
        authorization.outputChannelCount = 1U;
        authorization.staticParameterCount = parameters.size();
        authorization.staticParameterDigest =
            activation::calculateComponentActivationStaticParameterDigestV1(
                parameters);
        authorization.entryByteCount = entryBytes.size();
        authorization.artifactGeneration = 29U;
        authorization.activationGeneration = 31U;
        authorization.runtimeSessionGeneration =
            session.runtimeSessionGeneration;
        authorization.sharedStorageIdentity = session.sharedStorageIdentity;
        authorization.validationNonce.fill(0x5cU);
        authorization.runtimeSessionNonce = session.runtimeSessionNonce;
        authorization.authorizationNonce.fill(0xa5U);

        constexpr std::array tableValues{0.25, 0.5, 0.75};
        require(
            activation::encodeComponentActivationImmutableTableV1(
                tableValues, tableBytes)
                == activation::ComponentActivationProtocolDiagnosticV1::
                    Valid,
            "resource-set table should encode");
        initializeImmutableGrant(
            tableGrant,
            parameterIds[0],
            resourceIds[0],
            7U,
            tableBytes,
            false,
            tableValues.size());
        initializeImmutableGrant(
            binaryGrant,
            parameterIds[1],
            resourceIds[1],
            8U,
            binaryBytes,
            true,
            binaryBytes.size());
        for (std::size_t index = 0; index < audioGrants.size(); ++index)
        {
            initializeAudioGrant(
                audioGrants[index],
                parameterIds[index + 2U],
                resourceIds[index + 2U],
                9U + index,
                audioBytes[index]);
        }

        const std::array memberGrantDigests{
            recordDigest(tableGrant),
            recordDigest(binaryGrant),
            recordDigest(audioGrants[0]),
            recordDigest(audioGrants[1])};
        const std::array sourceDigests{
            tableGrant.sourceContentDigest,
            binaryGrant.sourceContentDigest,
            audioGrants[0].sourceContentDigest,
            audioGrants[1].sourceContentDigest};
        const std::array mediatedDigests{
            tableGrant.mediatedPayloadDigest,
            binaryGrant.mediatedPayloadDigest,
            audioGrants[0].sourceContentDigest,
            audioGrants[1].sourceContentDigest};
        const std::array<std::uint64_t, 4> sourceByteCounts{
            tableGrant.byteCount,
            binaryGrant.byteCount,
            audioGrants[0].sourceByteCount,
            audioGrants[1].sourceByteCount};
        const std::array<std::uint64_t, 4> generations{
            7U, 8U, 9U, 10U};
        const std::array kinds{
            activation::ComponentActivationResourceSetMemberKindV1::Table,
            activation::ComponentActivationResourceSetMemberKindV1::Binary,
            activation::ComponentActivationResourceSetMemberKindV1::Audio,
            activation::ComponentActivationResourceSetMemberKindV1::Audio};
        for (std::size_t index = 0; index < members.size(); ++index)
        {
            auto& member = members[index];
            member.header = activation::componentActivationHeaderV1(
                activation::ComponentActivationRecordKindV1::ResourceSetMember,
                sizeof(member));
            member.memberGrantDigest = memberGrantDigests[index];
            member.sourceContentDigest = sourceDigests[index];
            member.mediatedPayloadDigest = mediatedDigests[index];
            member.resourceGeneration = generations[index];
            member.sourceByteCount = sourceByteCounts[index];
            member.transportByteCount =
                roundedResourceSetBytes(sourceByteCounts[index]);
            member.privateMappedByteCount = member.transportByteCount;
            member.workingByteCount = index >= 2U
                ? activation::maximumComponentActivationAudioStreamWorkingBytesV1
                : 0U;
            member.parameterIdBytes = parameterIds[index].size();
            member.resourceIdBytes = resourceIds[index].size();
            member.memberKind = static_cast<std::uint32_t>(kinds[index]);
            member.memberOrdinal = static_cast<std::uint16_t>(index);
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
        setGrant.contentDigest = authorization.contentDigest;
        setGrant.validationNonce = authorization.validationNonce;
        setGrant.runtimeSessionNonce = authorization.runtimeSessionNonce;
        setGrant.authorizationNonce = authorization.authorizationNonce;
        setGrant.artifactGeneration = authorization.artifactGeneration;
        setGrant.activationGeneration = authorization.activationGeneration;
        setGrant.runtimeSessionGeneration =
            authorization.runtimeSessionGeneration;
        setGrant.sharedStorageIdentity = authorization.sharedStorageIdentity;
        setGrant.profileVersion =
            activation::componentActivationResourceSetProfileVersionV1;
        setGrant.memberCount = members.size();
        setGrant.audioStreamCount = audioGrants.size();
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

        preparedMembers[0] = {
            &members[0], &tableGrant, tableBytes, nullptr, {}};
        preparedMembers[1] = {
            &members[1], &binaryGrant, binaryBytes, nullptr, {}};
        preparedMembers[2] = {
            &members[2], nullptr, {}, &audioGrants[0], audioBytes[0]};
        preparedMembers[3] = {
            &members[3], nullptr, {}, &audioGrants[1], audioBytes[1]};
    }

private:
    void initializeImmutableGrant(
        activation::ComponentActivationImmutableResourceGrantV1& grant,
        std::string_view parameterId,
        std::string_view resourceId,
        std::uint64_t generation,
        std::span<const std::uint8_t> bytes,
        bool binary,
        std::uint64_t elementCount)
    {
        grant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::
                ImmutableResourceGrant,
            sizeof(grant));
        grant.contentDigest = authorization.contentDigest;
        grant.mediatedPayloadDigest =
            activation::calculateComponentActivationByteDigestV1(bytes);
        if (binary)
            grant.sourceContentDigest = grant.mediatedPayloadDigest;
        else
            grant.sourceContentDigest.fill(0x61U);
        grant.validationNonce = authorization.validationNonce;
        grant.runtimeSessionNonce = authorization.runtimeSessionNonce;
        grant.authorizationNonce = authorization.authorizationNonce;
        grant.artifactGeneration = authorization.artifactGeneration;
        grant.activationGeneration = authorization.activationGeneration;
        grant.runtimeSessionGeneration =
            authorization.runtimeSessionGeneration;
        grant.sharedStorageIdentity = authorization.sharedStorageIdentity;
        grant.resourceGeneration = generation;
        grant.elementCount = elementCount;
        grant.byteCount = bytes.size();
        grant.parameterIdBytes = parameterId.size();
        grant.resourceIdBytes = resourceId.size();
        grant.resourceKind = static_cast<std::uint32_t>(binary
                ? activation::ComponentActivationImmutableResourceKindV1::
                    Binary
                : activation::ComponentActivationImmutableResourceKindV1::
                    Table);
        grant.elementFormat = static_cast<std::uint32_t>(binary
                ? activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        RawBytes
                : activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
        std::copy(parameterId.begin(), parameterId.end(), grant.parameterId.begin());
        std::copy(resourceId.begin(), resourceId.end(), grant.resourceId.begin());
    }

    void initializeAudioGrant(
        activation::ComponentActivationAudioStreamGrantV1& grant,
        std::string_view parameterId,
        std::string_view resourceId,
        std::uint64_t generation,
        std::span<const std::uint8_t> bytes)
    {
        grant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::AudioStreamGrant,
            sizeof(grant));
        grant.contentDigest = authorization.contentDigest;
        grant.signerIdentityDigest = authorization.signerIdentityDigest;
        grant.sourceContentDigest =
            activation::calculateComponentActivationByteDigestV1(bytes);
        grant.validationNonce = authorization.validationNonce;
        grant.runtimeSessionNonce = authorization.runtimeSessionNonce;
        grant.authorizationNonce = authorization.authorizationNonce;
        grant.artifactGeneration = authorization.artifactGeneration;
        grant.activationGeneration = authorization.activationGeneration;
        grant.runtimeSessionGeneration =
            authorization.runtimeSessionGeneration;
        grant.sharedStorageIdentity = authorization.sharedStorageIdentity;
        grant.resourceGeneration = generation;
        grant.sourceByteCount = bytes.size();
        grant.sourceFrameCount = 2'048U;
        grant.maximumSourceFrameIncrement = 64.0;
        grant.parameterIdBytes = parameterId.size();
        grant.resourceIdBytes = resourceId.size();
        grant.sourceSampleRate = 48'000U;
        grant.streamProfileVersion =
            activation::componentActivationAudioStreamProfileVersionV1;
        grant.sourceEncoding = static_cast<std::uint32_t>(
            activation::ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
        grant.maximumReadFrames =
            activation::maximumComponentActivationAudioReadFramesV1;
        grant.decodeBlockFrames =
            activation::componentActivationAudioDecodeBlockFramesV1;
        grant.sourceChannelCount = 1U;
        grant.maximumTouchedSourceBlocks =
            activation::maximumComponentActivationAudioTouchedSourceBlocksV1;
        grant.requestCapacity =
            activation::componentActivationAudioRequestCapacityV1;
        grant.responseCapacity =
            activation::componentActivationAudioResponseCapacityV1;
        grant.workerCacheCapacity =
            activation::componentActivationAudioWorkerCacheCapacityV1;
        grant.lowWatermarkBlocks =
            activation::componentActivationAudioLowWatermarkBlocksV1;
        grant.highWatermarkBlocks =
            activation::componentActivationAudioHighWatermarkBlocksV1;
        std::copy(parameterId.begin(), parameterId.end(), grant.parameterId.begin());
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
            "/private/tmp/talas-audio-loader-materialization.XXXXXX";
        descriptor = mkstemp(fileTemplate);
        require(descriptor >= 0, "Audio materialization should allocate");
        path = fileTemplate;
        require(
            fchmod(descriptor, 0500) == 0,
            "Audio materialization should have exact mode");
    }

    ~MaterializationFile()
    {
        if (descriptor >= 0)
            static_cast<void>(close(descriptor));
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path, ignored));
    }

    int release() noexcept
    {
        const auto value = descriptor;
        descriptor = -1;
        return value;
    }
};

activation::ComponentActivationLoaderResultV1 loadFixture(
    AudioFixture& fixture,
    const activation::ComponentActivationAudioStreamGrantV1* grant,
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
        nullptr,
        {},
        grant,
        bytes);
}

activation::ComponentActivationLoaderResultV1 loadResourceSetFixture(
    ResourceSetLoaderFixture& fixture)
{
    MaterializationFile materialization;
    const activation::ComponentActivationPreparedResourceSetV1 set{
        &fixture.setGrant, fixture.preparedMembers};
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
        nullptr,
        {},
        nullptr,
        {},
        &set);
}

TalasKernelAbiProcessV1 processBlock(
    std::span<double> output,
    std::uint64_t absoluteFrame)
{
    static TalasKernelAbiSignalV1 input = [] {
        TalasKernelAbiSignalV1 value{};
        value.structSize = sizeof(value);
        value.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
        value.constantValue = 1.0;
        return value;
    }();
    static TalasKernelAbiOutputV1 outputRecord;
    outputRecord = {};
    outputRecord.structSize = sizeof(outputRecord);
    outputRecord.samples = output.data();
    outputRecord.sampleCount = output.size();
    TalasKernelAbiProcessV1 value{};
    value.structSize = sizeof(value);
    value.frameCount = output.size();
    value.inputs = &input;
    value.inputCount = 1U;
    value.outputs = &outputRecord;
    value.outputCount = 1U;
    value.transport.structSize = sizeof(value.transport);
    value.transport.absoluteFrame = absoluteFrame;
    return value;
}

void testExactAudioResolveAndOneReadPerProcessScope()
{
    AudioFixture fixture{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
    if (!loaded.ok())
    {
        std::cerr << "audio loader diagnostic="
                  << static_cast<unsigned>(loaded.diagnostic) << '\n';
    }
    require(loaded.ok(), "exact Audio fixture should resolve and prewarm");
    std::array<double, 7> output{};
    auto process = processBlock(output, 0U);
    require(
        loaded.instance->process(process, 1U) == TALAS_KERNEL_ABI_STATUS_OK,
        "one exact Audio read should be admitted in process scope");
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        require(
            std::abs(output[frame]
                - static_cast<double>(frame) / 32'767.0) < 1.0e-12,
            "Audio loader should preserve exact decoded source samples");
    }
    require(loaded.instance->shutdown(), "exact Audio loader should close cleanly");
}

void testExactResourceSetResolvesEverySlotAndReadsBothStreams()
{
    ResourceSetLoaderFixture fixture{
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE_PATH};
    auto loaded = loadResourceSetFixture(fixture);
    if (!loaded.ok())
    {
        std::cerr << "resource-set loader diagnostic="
                  << static_cast<unsigned>(loaded.diagnostic) << '\n';
    }
    require(
        loaded.ok(),
        "exact Table/Binary/two-Audio fixture should resolve and prewarm");
    const auto health = loaded.instance->audioStreamHealthSet();
    require(
        health.has_value()
            && health->binding.resourceSetDigest
                == fixture.setGrant.resourceSetDigest
            && health->slotCount == 2U
            && health->slots[0].binding.memberOrdinal == 2U
            && health->slots[1].binding.memberOrdinal == 3U,
        "resource-set loader should expose two independently bound health slots");
    const auto streamTokens = loaded.instance->audioStreamTokensForTesting();
    require(
        streamTokens[0] != 0U && streamTokens[1] != 0U
            && streamTokens[0] != streamTokens[1]
            && (streamTokens[0]
                & activation::componentActivationAudioStreamSetSlotMaskV1)
                == 1U
            && (streamTokens[1]
                & activation::componentActivationAudioStreamSetSlotMaskV1)
                == 2U,
        "each Audio view should carry its exact direct callback slot identity");
    std::array<double, 7> output{};
    auto process = processBlock(output, 0U);
    require(
        loaded.instance->process(process, 1U) == TALAS_KERNEL_ABI_STATUS_OK,
        "both exact Audio slots should read once in one process scope");
    constexpr std::array tableValues{0.25, 0.5, 0.75};
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto sample = static_cast<double>(frame) / 32'767.0;
        const auto binaryScale = static_cast<double>(
            fixture.binaryBytes[frame % fixture.binaryBytes.size()]) / 255.0;
        const auto expected = sample
            * (tableValues[frame % tableValues.size()] + binaryScale);
        require(
            std::abs(output[frame] - expected) < 1.0e-12,
            "resource-set process should consume every prepared member exactly");
    }
    require(
        loaded.instance->replaceAudioStreamEpochAndPrewarm(13.0),
        "resource-set epoch replacement should prewarm every Audio slot");
    require(
        loaded.instance->shutdown(),
        "resource-set loader should close every stream and mapping cleanly");
}

void testResourceSetScopeAndResolutionViolationsFailClosed()
{
    const auto processViolation = [](const char* path, const char* message) {
        ResourceSetLoaderFixture fixture{path};
        auto loaded = loadResourceSetFixture(fixture);
        require(loaded.ok(), "resource-set scope adversary should first prepare");
        std::array<double, 1> output{};
        auto process = processBlock(output, 0U);
        require(
            loaded.instance->process(process, 1U)
                == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
            message);
        require(
            !loaded.instance->shutdown(),
            "resource-set scope violation must remain latched");
    };
    processViolation(
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REPEATED_READ_FIXTURE_PATH,
        "a third/repeated resource-set Audio read must quarantine");
    processViolation(
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_INVALID_TOKEN_FIXTURE_PATH,
        "an unbound resource-set Audio token must quarantine");

    ResourceSetLoaderFixture missing{
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_MISSING_RESOLVE_FIXTURE_PATH};
    const auto loaded = loadResourceSetFixture(missing);
    require(
        !loaded.ok()
            && loaded.diagnostic
                == activation::ComponentActivationLoaderDiagnosticV1::
                    PrepareRejected,
        "missing one resource-set resolution must reject the complete prepare");
}

void testOuterWorkerPublishesEveryResourceSetHealthSlot()
{
    ResourceSetLoaderFixture fixture{
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE_PATH};
    auto loaded = loadResourceSetFixture(fixture);
    require(loaded.ok(), "resource-set worker fixture should prepare");
    const auto setHealth = loaded.instance->audioStreamHealthSet();
    require(
        setHealth.has_value() && setHealth->slotCount == 2U,
        "resource-set worker fixture should expose both Audio slots");

    struct alignas(64) SharedRuntimeStorage
    {
        std::array<std::byte, 64U * 1024U> bytes{};
    } storage;
    auto canaryPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioCanaryPlaneOffsetV1,
        activation::componentActivationAudioPlaneBytesV1};
    auto healthPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
        activation::componentActivationAudioStreamHealthPlaneBytesV2};
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            canaryPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready
            && activation::initializeComponentActivationAudioStreamHealthPlaneV2(
                healthPlane, fixture.authorization, setHealth->binding)
                == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "resource-set worker planes should initialize exactly");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        canaryPlane, fixture.authorization);
    auto health =
        activation::ComponentActivationAudioStreamHealthConsumerV2::attach(
            healthPlane, fixture.authorization, setHealth->binding);
    require(
        host.has_value() && health.has_value(),
        "resource-set worker endpoints should attach exactly");

    activation::ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
    auto worker = activation::ComponentActivationWorkerV1::prepare(
        canaryPlane,
        fixture.authorization,
        *loaded.instance,
        workerDiagnostic,
        healthPlane);
    require(
        worker != nullptr && worker->start(),
        "resource-set outer worker should start after all-stream prewarm");

    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{1U, 0, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "resource-set callback should publish without waiting");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(250);
    while (!host->publishedResponseReady()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    require(
        host->publishedResponseReady(),
        "resource-set worker should publish one exact response");
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{
                1U, 256, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "resource-set callback should consume the prepared response");
    const auto first = health->read(0U);
    const auto second = health->read(1U);
    require(
        first.has_value() && second.has_value()
            && first->seekGeneration != 0U
            && second->seekGeneration != 0U
            && first->decodeFailures == 0U
            && second->decodeFailures == 0U,
        "outer worker should publish two independently authenticated health slots");
    worker->stop();
    worker.reset();
    require(
        loaded.instance->shutdown(),
        "resource-set worker must stop before all stream mappings are released");
}

void testOneImpossibleResourceSetHealthSlotQuarantinesTheSet()
{
    ResourceSetLoaderFixture fixture{
        TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE_PATH};
    auto loaded = loadResourceSetFixture(fixture);
    require(loaded.ok(), "resource-set invalid-health fixture should prepare");
    const auto setHealth = loaded.instance->audioStreamHealthSet();
    require(setHealth.has_value(), "resource-set health binding should exist");

    struct alignas(64) SharedRuntimeStorage
    {
        std::array<std::byte, 64U * 1024U> bytes{};
    } storage;
    auto canaryPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioCanaryPlaneOffsetV1,
        activation::componentActivationAudioPlaneBytesV1};
    auto healthPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
        activation::componentActivationAudioStreamHealthPlaneBytesV2};
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            canaryPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready
            && activation::initializeComponentActivationAudioStreamHealthPlaneV2(
                healthPlane, fixture.authorization, setHealth->binding)
                == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "resource-set invalid-health planes should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        canaryPlane, fixture.authorization);
    require(host.has_value(), "resource-set invalid-health host should attach");

    activation::ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
    auto worker = activation::ComponentActivationWorkerV1::prepare(
        canaryPlane,
        fixture.authorization,
        *loaded.instance,
        workerDiagnostic,
        healthPlane);
    require(
        worker != nullptr && worker->start(),
        "resource-set invalid-health worker should start");
    loaded.instance->setAudioStreamHealthCountersForTest(1U, 1U);

    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{1U, 0, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "resource-set invalid-health callback should publish without waiting");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(250);
    while (!host->isQuarantined()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    require(
        host->isQuarantined()
            && host->quarantineReason()
                == activation::ComponentActivationAudioQuarantineReasonV1::
                    StreamHealthInvalid,
        "one impossible Audio health slot must quarantine the complete set");
    worker->stop();
    worker.reset();
    static_cast<void>(loaded.instance->shutdown());
}

void testDetachedAudioTupleRejectsBeforeLoading()
{
    AudioFixture missing{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    require(
        loadFixture(missing, nullptr, {}).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                AudioStreamRejected,
        "missing Audio grant/source must reject before loading");
    AudioFixture changed{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto changedBytes = changed.sourceBytes;
    changedBytes.back() ^= 0x01U;
    require(
        loadFixture(changed, &changed.grant, changedBytes).diagnostic
            == activation::ComponentActivationLoaderDiagnosticV1::
                AudioStreamRejected,
        "same-sized Audio source mutation must reject before loading");
}

void testScopeViolationsLatchQuarantine()
{
    const auto processViolation = [](const char* path, const char* message) {
        AudioFixture fixture{path};
        auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
        require(loaded.ok(), "scope adversary should first prepare");
        std::array<double, 1> output{};
        auto process = processBlock(output, 0U);
        require(
            loaded.instance->process(process, 1U)
                == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
            message);
        require(!loaded.instance->shutdown(), "scope misuse must remain latched");
    };
    processViolation(
        TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_LATE_RESOLVER_FIXTURE_PATH,
        "resolve after prepare must quarantine");
    processViolation(
        TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_REPEATED_READ_FIXTURE_PATH,
        "a second read in one process scope must quarantine");

    AudioFixture resetFixture{
        TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_RESET_READ_FIXTURE_PATH};
    auto resetLoaded = loadFixture(
        resetFixture, &resetFixture.grant, resetFixture.sourceBytes);
    require(resetLoaded.ok(), "reset-read adversary should prepare");
    TalasKernelAbiResetV1 reset{};
    reset.structSize = sizeof(reset);
    require(
        resetLoaded.instance->reset(reset) == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
        "read from reset scope must quarantine");
    require(!resetLoaded.instance->shutdown(), "reset misuse must stay latched");

    AudioFixture noSequence{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto noSequenceLoaded = loadFixture(
        noSequence, &noSequence.grant, noSequence.sourceBytes);
    require(noSequenceLoaded.ok(), "no-sequence adversary should prepare");
    std::array<double, 1> output{};
    auto process = processBlock(output, 0U);
    require(
        noSequenceLoaded.instance->process(process)
            == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
        "Audio process without an outer sequence must quarantine");
}

void testPrepareScopeIsExactAndOneShot()
{
    for (const auto path : {
             TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_OTHER_THREAD_FIXTURE_PATH,
             TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_DOUBLE_RESOLVE_FIXTURE_PATH,
             TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_MISSING_RESOLVE_FIXTURE_PATH})
    {
        AudioFixture fixture{path};
        const auto loaded = loadFixture(
            fixture, &fixture.grant, fixture.sourceBytes);
        require(
            !loaded.ok()
                && loaded.diagnostic
                    == activation::ComponentActivationLoaderDiagnosticV1::
                        PrepareRejected,
            "other-thread, repeated, or missing prepare resolution must reject");
    }
}

void testOuterWorkerPublishesStreamHealthWithoutCallbackBlocking()
{
    AudioFixture fixture{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
    require(loaded.ok(), "worker integration Audio fixture should prepare");

    struct alignas(64) SharedRuntimeStorage
    {
        std::array<std::byte, 64U * 1024U> bytes{};
    } storage;
    auto canaryPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioCanaryPlaneOffsetV1,
        activation::componentActivationAudioPlaneBytesV1};
    auto healthPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
        activation::componentActivationAudioStreamHealthPlaneBytesV1};
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            canaryPlane, fixture.authorization)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "worker integration audio plane should initialize");
    require(
        activation::initializeComponentActivationAudioStreamHealthPlaneV1(
            healthPlane, fixture.authorization)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "worker integration health plane should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        canaryPlane, fixture.authorization);
    auto health =
        activation::ComponentActivationAudioStreamHealthConsumerV1::attach(
            healthPlane, fixture.authorization);
    require(
        host.has_value() && health.has_value(),
        "worker integration endpoints should attach exactly");

    activation::ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
    auto worker = activation::ComponentActivationWorkerV1::prepare(
        canaryPlane,
        fixture.authorization,
        *loaded.instance,
        workerDiagnostic,
        healthPlane);
    require(
        worker != nullptr && worker->start(),
        "outer worker should start after a fresh stream epoch is prewarmed");

    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{1U, 0, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "first outer callback should publish without waiting");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(250);
    while (!host->publishedResponseReady()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    require(
        host->publishedResponseReady(),
        "outer worker should publish the prepared stream response");
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{
                1U, 256, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::Rendered,
        "next callback should consume one exact outer response");
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        require(
            std::abs(output[frame]
                - static_cast<float>(frame) / 32'767.0F) < 1.0e-6F,
            "outer response should preserve the decoded stream trace");
    }
    const auto observedHealth = health->read();
    require(
        observedHealth.has_value()
            && observedHealth->seekGeneration >= 2U
            && observedHealth->decodedBlocks != 0U
            && observedHealth->decodeFailures == 0U,
        "worker should publish one authenticated monotonic health snapshot");
    worker->stop();
    worker.reset();
    require(
        loaded.instance->shutdown(),
        "outer worker must stop before stream/instance teardown");
}

void testImpossibleStreamHealthFailsClosed()
{
    AudioFixture fixture{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
    require(loaded.ok(), "invalid-health fixture should prepare");

    struct alignas(64) SharedRuntimeStorage
    {
        std::array<std::byte, 64U * 1024U> bytes{};
    } storage;
    auto canaryPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioCanaryPlaneOffsetV1,
        activation::componentActivationAudioPlaneBytesV1};
    auto healthPlane = std::span<std::byte>{
        storage.bytes.data()
            + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
        activation::componentActivationAudioStreamHealthPlaneBytesV1};
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            canaryPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready
            && activation::initializeComponentActivationAudioStreamHealthPlaneV1(
                healthPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "invalid-health shared planes should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        canaryPlane, fixture.authorization);
    require(host.has_value(), "invalid-health host should attach");

    activation::ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
    auto worker = activation::ComponentActivationWorkerV1::prepare(
        canaryPlane,
        fixture.authorization,
        *loaded.instance,
        workerDiagnostic,
        healthPlane);
    require(worker != nullptr && worker->start(), "invalid-health worker should start");
    loaded.instance->setAudioStreamHealthCountersForTest(1U);

    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    require(
        host->process(
            inputs,
            outputs,
            output.size(),
            activation::ComponentActivationAudioTransportV1{1U, 0, 0U, 0U},
            true)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "invalid-health callback should publish without waiting");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(250);
    while (!host->isQuarantined()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    require(
        host->isQuarantined()
            && host->quarantineReason()
                == activation::ComponentActivationAudioQuarantineReasonV1::
                    StreamHealthInvalid,
        "impossible decoder health must quarantine before response publication");
    worker->stop();
    worker.reset();
    static_cast<void>(loaded.instance->shutdown());
}
}

namespace
{
bool sameSetHealth(const std::optional<activation::ComponentActivationAudioStreamHealthSetV1>& a,
    const std::optional<activation::ComponentActivationAudioStreamHealthSetV1>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->binding != b->binding || a->slotCount != b->slotCount) return false;
    for (std::size_t i = 0; i < a->slotCount; ++i)
        if (a->slots[i].binding != b->slots[i].binding || a->slots[i].health != b->slots[i].health) return false;
    return true;
}

template<class Worker>
void testActualWorkerCheckpoint()
{
    if constexpr (!requires(Worker& worker) {
        worker.beginOfflineCheckpoint(std::uint64_t{});
        worker.restoreOfflineCheckpoint(std::uint64_t{});
        worker.commitOfflineCheckpoint(std::uint64_t{});
    })
    {
        AudioFixture fixture{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
        auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
        require(loaded.ok() && loaded.instance->supportsOfflineCheckpoint(),
            "real worker checkpoint fixture must first load with coordinated resources");
        require(false, "actual worker lacks a matching-sequence kernel/resource/transport checkpoint");
    }
    else
    {
        using HostResult = activation::ComponentActivationAudioCheckpointResultV1;
        using Operation = activation::ComponentActivationAudioOperationV1;
        for (bool resourceSet : {false, true})
        for (bool prefix : {false, true})
        {
            AudioFixture single{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
            ResourceSetLoaderFixture multiple{TALAS_RESOURCE_CHECKPOINT_FIXTURE_52};
            auto loaded = resourceSet ? loadResourceSetFixture(multiple)
                : loadFixture(single, &single.grant, single.sourceBytes);
            require(loaded.ok(), "real checkpoint worker should load its recursive resource fixture");
            auto& instance = *loaded.instance;
            const auto& authority = resourceSet ? multiple.authorization : single.authorization;
            struct alignas(64) Storage { std::array<std::byte, 64U * 1024U> bytes{}; } storage;
            auto audioPlane = std::span<std::byte>{storage.bytes.data()
                + activation::componentActivationAudioCanaryPlaneOffsetV1,
                activation::componentActivationAudioPlaneBytesV1};
            auto healthPlane = std::span<std::byte>{storage.bytes.data()
                + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
                resourceSet ? activation::componentActivationAudioStreamHealthPlaneBytesV2
                            : activation::componentActivationAudioStreamHealthPlaneBytesV1};
            require(activation::initializeComponentActivationAudioDataPlaneV1(audioPlane, authority)
                == activation::ComponentActivationAudioDiagnosticV1::Ready, "worker checkpoint audio plane initializes");
            const auto initializedHealth = resourceSet
                ? activation::initializeComponentActivationAudioStreamHealthPlaneV2(healthPlane, authority, instance.audioStreamHealthSet()->binding)
                : activation::initializeComponentActivationAudioStreamHealthPlaneV1(healthPlane, authority);
            require(initializedHealth == activation::ComponentActivationAudioDiagnosticV1::Ready, "worker checkpoint health plane initializes");
            auto host = activation::ComponentActivationAudioHostV1::attach(audioPlane, authority);
            activation::ComponentActivationWorkerDiagnosticV1 diagnostic;
            auto worker = Worker::prepare(audioPlane, authority, instance, diagnostic, healthPlane);
            require(host && worker, "real worker checkpoint endpoints prepare");
            std::optional<activation::ComponentActivationAudioStreamHealthConsumerV1> consumer1;
            std::optional<activation::ComponentActivationAudioStreamHealthConsumerV2> consumer2;
            if (resourceSet) consumer2 = activation::ComponentActivationAudioStreamHealthConsumerV2::attach(
                healthPlane, authority, instance.audioStreamHealthSet()->binding);
            else consumer1 = activation::ComponentActivationAudioStreamHealthConsumerV1::attach(healthPlane, authority);
            require(resourceSet ? consumer2.has_value() : consumer1.has_value(), "real checkpoint health consumer attaches");
            std::array<float, 256> input{}, output{}, pending{};
            input.fill(1.0F);
            const std::array<const float*, 1> inputs{input.data()};
            const std::array<float*, 1> outputs{output.data()};
            bool hasPending = false;
            double history = 0;
            std::uint64_t generation = 0, physicalSequence = 0;
            auto awaitResponse = [&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (!host->publishedResponseReady() && !host->isQuarantined()
                    && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                require(host->publishedResponseReady() && !host->isQuarantined(), "actual worker completes the exact request before checkpoint control");
                for (std::size_t slot = 0; slot < (resourceSet ? 2U : 1U); ++slot)
                    require((resourceSet ? consumer2->read(slot) : consumer1->read()).has_value(),
                        "real health consumer accepts monotonic publication across provisional and restored work");
            };
            auto healthPublications = [&] {
                std::array<std::uint64_t, 2> result{};
                for (std::size_t slot = 0; slot < (resourceSet ? 2U : 1U); ++slot)
                {
                    auto& publication = resourceSet
                        ? reinterpret_cast<activation::ComponentActivationAudioStreamHealthSharedPlaneV2*>(healthPlane.data())->slots[slot].publication
                        : reinterpret_cast<activation::ComponentActivationAudioStreamHealthSharedPlaneV1*>(healthPlane.data())->publication;
                    result[slot] = std::atomic_ref{publication}.load(std::memory_order_acquire);
                }
                return result;
            };
            auto requireFreshHealth = [&](const auto& previous) {
                const auto current = healthPublications();
                for (std::size_t slot = 0; slot < (resourceSet ? 2U : 1U); ++slot)
                    require(current[slot] == previous[slot] + 2,
                        "each checkpoint control publishes exactly one fresh physical health record per stream");
            };
            auto checkPublishedHealth = [&] {
                const auto singleHealth = instance.audioStreamHealth();
                const auto setHealth = instance.audioStreamHealthSet();
                for (std::size_t slot = 0; slot < (resourceSet ? 2U : 1U); ++slot)
                    require((resourceSet ? consumer2->read(slot) : consumer1->read())
                        == (resourceSet ? std::optional{setHealth->slots[slot].health} : singleHealth),
                        "checkpoint control publishes exact resource health without another audio block");
            };
            auto submit = [&](std::uint64_t position, std::uint64_t epoch, bool complete = true) {
                if (complete && !worker->running()) require(worker->start(), "same worker restarts without replacing stream epochs");
                const auto operation = host->process(inputs, outputs, 256,
                    {epoch, static_cast<std::int64_t>(position), 0, 0}, true);
                require(operation == (hasPending ? Operation::Rendered : Operation::FirstBlockSilence),
                    "real worker rollback retains exact one-block logical latency");
                require(host->lastSubmittedSequence() == ++physicalSequence, "real worker rollback never rewinds physical sequences");
                for (std::size_t frame = 0; frame < output.size(); ++frame)
                {
                    const auto expected = hasPending ? pending[frame] : 0.0F;
                    if (std::abs(output[frame] - expected) >= 1e-6F)
                        std::cerr << "worker checkpoint mismatch: set=" << resourceSet
                            << " prefix=" << prefix << " sequence=" << physicalSequence
                            << " position=" << position << " epoch=" << epoch
                            << " frame=" << frame << " actual=" << output[frame]
                            << " expected=" << expected << '\n';
                    require(std::abs(output[frame] - expected) < 1e-6F,
                        "actual worker delayed audio must match independent recursive source history");
                }
                if (generation == 0 || generation != epoch) history = 0;
                generation = epoch;
                constexpr std::array table{0.25, 0.5, 0.75};
                for (std::size_t frame = 0; frame < pending.size(); ++frame)
                {
                    auto sample = static_cast<double>(position + frame) / 32767.0;
                    if (resourceSet) sample *= table[frame % table.size()]
                        + static_cast<double>(multiple.binaryBytes[frame % multiple.binaryBytes.size()]) / 255.0;
                    history = history * 0.5 + sample;
                    pending[frame] = static_cast<float>(history);
                }
                hasPending = true;
                if (complete) awaitResponse();
            };
            if (prefix) submit(0, 7);
            std::uint64_t nextPosition = prefix ? 256 : 0;
            for (std::size_t attempt = 0; attempt < 16; ++attempt)
            {
                // A source seek has its existing explicit prewarm boundary;
                // accidental unprepared discontinuity silence is not the oracle.
                worker->stop();
                if (nextPosition >= 1024) nextPosition = 0;
                require(instance.replaceAudioStreamEpochAndPrewarm(nextPosition), "baseline source window prewarms before capture");
                const auto savedHistory = history;
                const auto savedGeneration = generation;
                const auto savedPending = pending;
                const auto savedHasPending = hasPending;
                require(host->beginOfflineCheckpoint(true) == HostResult::Captured, "host captures matching logical output");
                const auto capturePublications = healthPublications();
                require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK
                    && !worker->running(), "actual worker captures only after joining at exact response boundary");
                requireFreshHealth(capturePublications);
                require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                    "nested worker capture must retain original ownership");
                checkPublishedHealth();
                const auto health = instance.audioStreamHealth();
                const auto healthSet = instance.audioStreamHealthSet();
                for (std::size_t provisional = 0; provisional < 4; ++provisional) submit(nextPosition + provisional * 256, 9);
                require(instance.audioStreamHealth() == health && sameSetHealth(instance.audioStreamHealthSet(), healthSet),
                    "actual provisional worker retains captured aggregate resource health");
                for (std::size_t slot = 0; slot < (resourceSet ? 2U : 1U); ++slot)
                    require((resourceSet ? consumer2->read(slot) : consumer1->read())
                        == (resourceSet ? std::optional{healthSet->slots[slot].health} : health),
                        "shared health carries captured resource state without rewinding its publication sequence");
                const auto restorePublications = healthPublications();
                const auto before = ordinaryAllocations;
                const auto status = worker->restoreOfflineCheckpoint(physicalSequence);
                const auto after = ordinaryAllocations;
                require(status == TALAS_KERNEL_ABI_STATUS_OK && before == after && !worker->running(),
                    "actual worker restores kernel/resources/transport without allocation while stopped");
                requireFreshHealth(restorePublications);
                checkPublishedHealth();
                require(host->restoreOfflineCheckpoint(true) == HostResult::Restored, "host restores the same completed boundary");
                history = savedHistory; generation = savedGeneration; pending = savedPending; hasPending = savedHasPending;
                submit(nextPosition, 7);
                submit(nextPosition + 256, 7); // Consume post-restore DSP, not only cached host audio.
                nextPosition += 512;
            }
            require(host->beginOfflineCheckpoint(true) == HostResult::Captured
                && worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK, "real commit baseline captures");
            submit(nextPosition, 11);
            const auto commitPublications = healthPublications();
            const auto before = ordinaryAllocations;
            const auto committed = worker->commitOfflineCheckpoint(physicalSequence);
            const auto after = ordinaryAllocations;
            require(committed == TALAS_KERNEL_ABI_STATUS_OK && before == after
                && host->commitOfflineCheckpoint(true) == HostResult::Committed, "actual worker commit preserves provisional history without allocation");
            requireFreshHealth(commitPublications);
            checkPublishedHealth();
            submit(nextPosition + 256, 11); submit(nextPosition + 512, 11);
            // Stop before publishing the next request: both stale completed
            // identity and latest-but-unprocessed identity must reject capture.
            worker->stop();
            require(instance.replaceAudioStreamEpochAndPrewarm(0), "boundary retry source prewarms");
            submit(0, 13, false);
            require(worker->beginOfflineCheckpoint(physicalSequence - 1) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                && worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                && !host->isQuarantined(), "stopped worker with queued request cannot capture either boundary");
            require(worker->start(), "boundary rejection leaves worker restartable");
            awaitResponse();
            require(worker->beginOfflineCheckpoint(physicalSequence + 1) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                "future expected completion cannot capture");
            require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK,
                "completed request permits exact boundary retry");
            require(worker->restoreOfflineCheckpoint(physicalSequence - 1) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                && worker->commitOfflineCheckpoint(physicalSequence + 1) == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                && worker->commitOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK,
                "wrong restore/commit boundaries preserve the held snapshot for exact retry");
            for (std::size_t site = 0; site < (resourceSet ? 2U : 1U); ++site)
            {
                allocationFaultTriggered = false;
                allocationFaultCountdown = site;
                const auto failed = worker->beginOfflineCheckpoint(physicalSequence);
                allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
                require(allocationFaultTriggered && failed == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR
                    && instance.active() && !host->isQuarantined(), "actual worker resource capture failure unwinds and stays usable");
                require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK
                    && worker->commitOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK,
                    "actual worker retries each resource capture allocation failure");
            }
            instance.failNextCheckpointAllocationForTesting();
            require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR
                && instance.active() && !host->isQuarantined(), "actual worker kernel capture failure unwinds resources without losing authority");
            require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK
                && worker->commitOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK,
                "actual worker retries kernel allocation failure");
            submit(256, 13); // Consume delayed input from the boundary retry.
            require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_OK,
                "terminal/lifecycle baseline captures on the actual worker");
            if (prefix)
            {
                host->quarantine();
                if (resourceSet)
                    require(worker->beginOfflineCheckpoint(physicalSequence) == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
                        "lost authority overrides nested worker capture and discards ownership");
                const auto terminal = resourceSet ? worker->commitOfflineCheckpoint(physicalSequence)
                    : worker->restoreOfflineCheckpoint(physicalSequence);
                require(terminal == TALAS_KERNEL_ABI_STATUS_QUARANTINED && !worker->start(),
                    "terminal plane authority discards held worker state and forbids restart");
                worker.reset();
                require(instance.shutdown(), "external plane revocation still cleans a healthy loaded instance");
            }
            else if (resourceSet)
            {
                instance.setAudioStreamHealthCountersForTest(1, 1);
                require(worker->restoreOfflineCheckpoint(physicalSequence) != TALAS_KERNEL_ABI_STATUS_OK
                    && !instance.active() && host->isQuarantined() && !worker->start(),
                    "one failed resource prevents real worker rollback and restart");
                worker.reset();
                require(!instance.shutdown(), "terminal resource failure stays visible through worker retirement");
            }
            else
            {
                worker.reset();
                require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK
                    && instance.commitOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                    "worker destruction consumes held ownership before leaving loaded instance alive");
                require(instance.shutdown(), "actual checkpoint worker retires before its loaded resources");
            }

        }
        for (bool resourceSet : {false, true})
        for (unsigned operation = 0; operation < 3; ++operation)
        for (bool exhausted : {false, true})
        {
            AudioFixture single{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
            ResourceSetLoaderFixture multiple{TALAS_RESOURCE_CHECKPOINT_FIXTURE_52};
            auto loaded = resourceSet ? loadResourceSetFixture(multiple)
                : loadFixture(single, &single.grant, single.sourceBytes);
            require(loaded.ok(), "health publication failure fixture loads");
            auto& instance = *loaded.instance;
            const auto& authority = resourceSet ? multiple.authorization : single.authorization;
            struct alignas(64) Storage { std::array<std::byte, 64U * 1024U> bytes{}; } storage;
            auto audioPlane = std::span<std::byte>{storage.bytes.data() + activation::componentActivationAudioCanaryPlaneOffsetV1,
                activation::componentActivationAudioPlaneBytesV1};
            auto healthPlane = std::span<std::byte>{storage.bytes.data() + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
                resourceSet ? activation::componentActivationAudioStreamHealthPlaneBytesV2 : activation::componentActivationAudioStreamHealthPlaneBytesV1};
            require(activation::initializeComponentActivationAudioDataPlaneV1(audioPlane, authority)
                == activation::ComponentActivationAudioDiagnosticV1::Ready, "failure audio plane initializes");
            require((resourceSet ? activation::initializeComponentActivationAudioStreamHealthPlaneV2(healthPlane, authority, instance.audioStreamHealthSet()->binding)
                : activation::initializeComponentActivationAudioStreamHealthPlaneV1(healthPlane, authority))
                == activation::ComponentActivationAudioDiagnosticV1::Ready, "failure health plane initializes");
            auto host = activation::ComponentActivationAudioHostV1::attach(audioPlane, authority);
            activation::ComponentActivationWorkerDiagnosticV1 diagnostic;
            auto worker = Worker::prepare(audioPlane, authority, instance, diagnostic, healthPlane);
            require(host && worker, "failure checkpoint endpoints prepare");
            if (operation != 0) require(worker->beginOfflineCheckpoint(0) == TALAS_KERNEL_ABI_STATUS_OK,
                "publication failure starts with held ownership");
            auto& publication = resourceSet
                ? reinterpret_cast<activation::ComponentActivationAudioStreamHealthSharedPlaneV2*>(healthPlane.data())->slots[1].publication
                : reinterpret_cast<activation::ComponentActivationAudioStreamHealthSharedPlaneV1*>(healthPlane.data())->publication;
            std::atomic_ref{publication}.store(exhausted ? std::numeric_limits<std::uint64_t>::max() - 1 : 1, std::memory_order_release);
            const auto status = operation == 0 ? worker->beginOfflineCheckpoint(0)
                : operation == 1 ? worker->restoreOfflineCheckpoint(0) : worker->commitOfflineCheckpoint(0);
            require(status == TALAS_KERNEL_ABI_STATUS_QUARANTINED && host->isQuarantined()
                && host->quarantineReason() == activation::ComponentActivationAudioQuarantineReasonV1::StreamHealthInvalid
                && !worker->start(), "odd or exhausted health publication is terminal at every checkpoint boundary");
            require(instance.active() && instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK
                && instance.commitOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                "failed health publication consumes worker-owned snapshots without reviving plane authority");
            worker.reset();
            require(instance.shutdown(), "publication failure still retires all loaded state");
        }
        for (bool restoreFailure : {false, true})
        {
            AudioFixture fixture{restoreFailure ? TALAS_RESOURCE_CHECKPOINT_RESTORE_FAILURE
                : TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
            auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
            require(loaded.ok(), "worker capability/failure fixture loads");
            struct alignas(64) Storage { std::array<std::byte, 64U * 1024U> bytes{}; } storage;
            auto audioPlane = std::span<std::byte>{storage.bytes.data()
                + activation::componentActivationAudioCanaryPlaneOffsetV1,
                activation::componentActivationAudioPlaneBytesV1};
            auto healthPlane = std::span<std::byte>{storage.bytes.data()
                + activation::componentActivationAudioStreamHealthPlaneOffsetV1,
                activation::componentActivationAudioStreamHealthPlaneBytesV1};
            require(activation::initializeComponentActivationAudioDataPlaneV1(audioPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready
                && activation::initializeComponentActivationAudioStreamHealthPlaneV1(healthPlane, fixture.authorization)
                == activation::ComponentActivationAudioDiagnosticV1::Ready, "worker capability/failure planes initialize");
            auto host = activation::ComponentActivationAudioHostV1::attach(audioPlane, fixture.authorization);
            activation::ComponentActivationWorkerDiagnosticV1 diagnostic;
            auto worker = Worker::prepare(audioPlane, fixture.authorization, *loaded.instance, diagnostic, healthPlane);
            require(host && worker, "worker capability/failure endpoints prepare");
            const auto capture = worker->beginOfflineCheckpoint(0);
            require(capture == (restoreFailure ? TALAS_KERNEL_ABI_STATUS_OK : TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE),
                "legacy absent extension stays unsupported while valid kernel captures");
            require(worker->start(), "legacy refusal or captured state permits ordinary worker start");
            std::array<float, 256> input{}, output{};
            const std::array<const float*, 1> inputs{input.data()};
            const std::array<float*, 1> outputs{output.data()};
            require(host->process(inputs, outputs, 256, {1, 0, 0, 0}, true) == Operation::FirstBlockSilence,
                "capability/failure fixture executes after checkpoint decision");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!host->publishedResponseReady() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            require(host->publishedResponseReady(), "capability/failure fixture publishes its exact response");
            if (restoreFailure)
                require(worker->restoreOfflineCheckpoint(1) == TALAS_KERNEL_ABI_STATUS_FATAL
                    && host->isQuarantined() && !loaded.instance->active() && !worker->start(),
                    "kernel restoration failure quarantines the real worker and forbids resurrection");
            worker->stop(); worker.reset();
            require(loaded.instance->shutdown(), "worker capability/failure fixture releases every snapshot cleanly");
        }
    }
}

template<class Loaded>
void testCombinedLoadedCheckpoint()
{
    if constexpr (!requires(Loaded& instance) {
        instance.supportsOfflineCheckpoint(); instance.beginOfflineCheckpoint();
        instance.restoreOfflineCheckpoint(); instance.commitOfflineCheckpoint();
    })
    {
        AudioFixture fixture{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
        auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
        require(loaded.ok() && loaded.instance->supportsKernelOfflineCheckpoint(),
            "stateful resource fixture must pass authenticated loading and kernel discovery");
        require(false, "loaded component lacks coordinated kernel/resource checkpoint API");
    }
    else
    {
        for (bool resourceSet : {false, true})
        for (std::uint64_t prefix : {0U, 7U})
        {
            AudioFixture single{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
            ResourceSetLoaderFixture multiple{TALAS_RESOURCE_CHECKPOINT_FIXTURE_52};
            auto loaded = resourceSet ? loadResourceSetFixture(multiple)
                : loadFixture(single, &single.grant, single.sourceBytes);
            require(loaded.ok(), "stateful combined checkpoint fixture must prepare");
            Loaded& instance = *loaded.instance;
            require(instance.supportsOfflineCheckpoint(), "resource checkpoint capability must be explicit");
            std::uint64_t sequence = 1;
            double expectedHistory = 0;
            auto check = [&](std::uint64_t position, double& history) {
                std::array<double, 7> output{};
                auto block = processBlock(output, position);
                require(instance.process(block, sequence++) == TALAS_KERNEL_ABI_STATUS_OK,
                    "combined fixture should process using a new outer sequence");
                constexpr std::array table{0.25, 0.5, 0.75};
                for (std::size_t frame = 0; frame < output.size(); ++frame)
                {
                    auto sample = static_cast<double>(position + frame) / 32767.0;
                    if (resourceSet) sample *= table[frame % table.size()]
                        + static_cast<double>(multiple.binaryBytes[frame % multiple.binaryBytes.size()]) / 255.0;
                    history = history * 0.5 + sample;
                    require(std::abs(output[frame] - history) < 1e-12,
                        "combined restore must match independent source and recursive kernel history");
                }
            };
            if (prefix) check(0, expectedHistory);
            const auto tokens = instance.audioStreamTokensForTesting();
            for (std::size_t attempt = 0; attempt < 20; ++attempt)
            {
                require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                    "combined checkpoint should capture kernel and all resources");
                require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                    "nested combined capture must retain baseline");
                require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                    && instance.restoreKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                    && instance.commitKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                    "kernel-only calls cannot consume coordinated ownership");
                const auto health = instance.audioStreamHealth();
                const auto setHealth = instance.audioStreamHealthSet();
                auto provisionalHistory = expectedHistory;
                check(prefix, provisionalHistory);
                require(instance.replaceAudioStreamEpochAndPrewarm(1900), "provisional source seek should prewarm");
                std::array<double, 7> provisional{};
                require(instance.process(processBlock(provisional, 1900), sequence++) == TALAS_KERNEL_ABI_STATUS_OK,
                    "provisional source window must execute");
                require(instance.audioStreamHealth() == health && sameSetHealth(instance.audioStreamHealthSet(), setHealth),
                    "all provisional resource health must remain at the captured aggregate boundary");
                const auto beforeRestore = ordinaryAllocations;
                const auto restored = instance.restoreOfflineCheckpoint();
                const auto afterRestore = ordinaryAllocations;
                require(restored == TALAS_KERNEL_ABI_STATUS_OK && beforeRestore == afterRestore,
                    "combined restore must consume every participant without allocation");
                require(instance.audioStreamTokensForTesting() == tokens, "combined restore must retain stream tokens");
                check(prefix, expectedHistory);
                prefix += 7;
            }
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                "combined commit should capture");
            check(prefix, expectedHistory);
            prefix += 7;
            const auto beforeCommit = ordinaryAllocations;
            const auto committed = instance.commitOfflineCheckpoint();
            const auto afterCommit = ordinaryAllocations;
            require(committed == TALAS_KERNEL_ABI_STATUS_OK && beforeCommit == afterCommit,
                "combined commit must retain all processing histories without allocation");
            check(prefix, expectedHistory);
            prefix += 7;
            const auto beforeCapture = ordinaryAllocations;
            const auto measured = instance.beginOfflineCheckpoint();
            const auto sites = ordinaryAllocations - beforeCapture;
            require(measured == TALAS_KERNEL_ABI_STATUS_OK && sites == (resourceSet ? 2U : 1U),
                "capture should expose one ordinary allocation per resource participant");
            const auto baselineHealth = instance.audioStreamHealth();
            const auto baselineSet = instance.audioStreamHealthSet();
            require(instance.commitOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "measurement baseline should commit");
            for (std::size_t site = 0; site < sites; ++site)
            {
                allocationFaultTriggered = false;
                allocationFaultCountdown = site;
                const auto failed = instance.beginOfflineCheckpoint();
                allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
                require(allocationFaultTriggered && failed == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR && instance.active()
                    && instance.audioStreamHealth() == baselineHealth && sameSetHealth(instance.audioStreamHealthSet(), baselineSet),
                    "partial resource capture failure must unwind earlier participants and preserve aggregate health");
                require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "partial capture failure must permit retry");
                auto retryHistory = expectedHistory;
                check(prefix, retryHistory);
                require(instance.restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "resource allocation retry should restore");
            }
            instance.failNextCheckpointAllocationForTesting();
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR && instance.active()
                && instance.audioStreamHealth() == baselineHealth && sameSetHealth(instance.audioStreamHealthSet(), baselineSet),
                "kernel capture allocation failure must unwind every captured resource");
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "kernel allocation failure must permit retry");
            std::atomic<std::size_t> inspections{0};
            std::atomic<bool> inconsistent{false};
            std::jthread observer([&](std::stop_token stop) {
                while (!stop.stop_requested())
                {
                    if (instance.audioStreamHealth() != baselineHealth
                        || !sameSetHealth(instance.audioStreamHealthSet(), baselineSet)) inconsistent.store(true);
                    ++inspections;
                    std::this_thread::yield();
                }
            });
            while (inspections.load() == 0) std::this_thread::yield();
            for (std::size_t i = 0; i < 8; ++i)
            {
                require(instance.replaceAudioStreamEpochAndPrewarm(1900), "concurrent inspection fixture should advance resources");
                std::array<double, 7> output{};
                require(instance.process(processBlock(output, 1900), sequence++) == TALAS_KERNEL_ABI_STATUS_OK,
                    "concurrent inspection fixture should advance kernel state");
            }
            require(instance.restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "aggregate publication should restore atomically");
            observer.request_stop();
            observer.join();
            require(!inconsistent.load(), "concurrent aggregate health must never expose mixed provisional slots");
            auto retryHistory = expectedHistory;
            check(prefix, retryHistory);
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "shutdown baseline should capture");
            require(instance.shutdown() && instance.restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                "shutdown must consume coordinated checkpoints and close cleanly");
        }
    }
}
}

namespace
{
void testCombinedCheckpointFailureAuthorityAndLegacy()
{
    AudioFixture legacy{TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH};
    auto original = loadFixture(legacy, &legacy.grant, legacy.sourceBytes);
    require(original.ok() && !original.instance->supportsOfflineCheckpoint()
        && original.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE
        && original.instance->active(), "legacy resource modules must retain absent checkpoint capability");
    require(original.instance->shutdown(), "legacy module must close cleanly");
    {
        AudioFixture fixture{TALAS_RESOURCE_CHECKPOINT_RESTORE_FAILURE};
        auto loaded = loadFixture(fixture, &fixture.grant, fixture.sourceBytes);
        require(loaded.ok() && loaded.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
            "kernel restore adversary should capture all participants");
        const auto baseline = loaded.instance->audioStreamHealth();
        require(loaded.instance->replaceAudioStreamEpochAndPrewarm(1900), "restore adversary should advance resources");
        require(loaded.instance->restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_FATAL && !loaded.instance->active(),
            "failed kernel restoration must keep the complete loaded instance terminal");
        const auto after = loaded.instance->audioStreamHealth();
        require(after && baseline && after->seekGeneration > baseline->seekGeneration,
            "kernel restoration failure must discard rather than rewind otherwise healthy resources");
        require(loaded.instance->shutdown(), "kernel restore failure must still release every checkpoint cleanly");
    }
    for (bool resourceSet : {false, true})
    for (bool commit : {false, true})
    {
        AudioFixture single{TALAS_RESOURCE_CHECKPOINT_FIXTURE_39};
        ResourceSetLoaderFixture multiple{TALAS_RESOURCE_CHECKPOINT_FIXTURE_52};
        auto loaded = resourceSet ? loadResourceSetFixture(multiple)
            : loadFixture(single, &single.grant, single.sourceBytes);
        require(loaded.ok(), "terminal resource fixture must prepare");
        auto& instance = *loaded.instance;
        require(instance.beginKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK
            && instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
            && instance.commitKernelOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
            "coordinated capture must not replace a kernel-only baseline");
        require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "terminal resource fixture must capture");
        instance.setAudioStreamHealthCountersForTest(1, resourceSet ? std::optional<std::size_t>{1} : std::nullopt);
        if (resourceSet)
        {
            const auto health = instance.audioStreamHealthSet();
            require(health && health->slots[0].health.decodeFailures == 0 && health->slots[1].health.decodeFailures == 1,
                "one impossible resource slot must override aggregate health deferral");
        }
        else require(instance.audioStreamHealth()->decodeFailures == 1,
            "impossible stream health must override aggregate deferral");
        const auto status = commit ? instance.commitOfflineCheckpoint() : instance.restoreOfflineCheckpoint();
        require(status != TALAS_KERNEL_ABI_STATUS_OK && !instance.active()
            && instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
            "commit or restore must discard snapshots without reviving an impossible resource");
        require(!instance.shutdown(), "resource failure authority must remain visible through shutdown");
    }
}
}

int main()
{
    testActualWorkerCheckpoint<activation::ComponentActivationWorkerV1>();
    testExactAudioResolveAndOneReadPerProcessScope();
    testExactResourceSetResolvesEverySlotAndReadsBothStreams();
    testResourceSetScopeAndResolutionViolationsFailClosed();
    testOuterWorkerPublishesEveryResourceSetHealthSlot();
    testOneImpossibleResourceSetHealthSlotQuarantinesTheSet();
    testDetachedAudioTupleRejectsBeforeLoading();
    testScopeViolationsLatchQuarantine();
    testPrepareScopeIsExactAndOneShot();
    testOuterWorkerPublishesStreamHealthWithoutCallbackBlocking();
    testImpossibleStreamHealthFailsClosed();
    testCombinedLoadedCheckpoint<activation::ComponentActivationLoadedInstanceV1>();
    testCombinedCheckpointFailureAuthorityAndLegacy();
    return 0;
}
