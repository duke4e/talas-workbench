#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/IsolatedComponentRuntime.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleReplacementConfirmation.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace talas::runtime
{
struct RealtimeRenderSessionTestAccess
{
    static auto beginOfflineTransaction(RealtimeRenderSession& session)
    {
        return session.beginOfflineTransaction();
    }
};
}

namespace
{
thread_local bool countOfflineAllocations = false;
thread_local std::size_t offlineAllocationCount = 0;
thread_local std::size_t offlineFaultCountdown = std::numeric_limits<std::size_t>::max();
thread_local bool offlineFaultTriggered = false;
void recordOfflineAllocation()
{
    if (countOfflineAllocations) ++offlineAllocationCount;
    if (offlineFaultCountdown == std::numeric_limits<std::size_t>::max()) return;
    if (offlineFaultCountdown-- == 0)
    {
        offlineFaultCountdown = std::numeric_limits<std::size_t>::max();
        offlineFaultTriggered = true;
        throw std::bad_alloc{};
    }
}
}
void* operator new(std::size_t size)
{
    recordOfflineAllocation();
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
    recordOfflineAllocation();
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
constexpr std::size_t blockFrames = 256;
constexpr double sampleRate = 48000.0;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression scalarParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression booleanParameter(
    std::string id,
    bool value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.boolean = value;
    return result;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resourceId)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.resource = std::move(resourceId);
    return result;
}

talas::project::ProjectPreservedPort audioPort(std::string id, bool required)
{
    auto signal = talas::core::defaultPortSignalContract(
        talas::core::PortType::Audio);
    signal.rate = talas::core::PortSignalRate::Sample;
    signal.audioLayout = talas::core::AudioBusLayoutKind::Mono;
    signal.edge = talas::core::PortEdgeSemantics::Continuous;
    signal.timelineBearing = true;
    return {
        std::move(id),
        talas::core::PortType::Audio,
        required,
        signal,
    };
}

talas::project::ProjectDocument isolatedDocument(std::string gain = "0.5")
{
    talas::project::ProjectPreservedComponent capsule;
    capsule.typeVersion = "1.0.0";
    capsule.stateSchemaVersion = 1;
    capsule.displayName = "Prepared isolated gain";
    capsule.bundleId = "org.talas.activation.fixture";
    capsule.inputs = {audioPort("in0", true)};
    capsule.outputs = {audioPort("out0", true)};
    capsule.parameters.push_back({
        "gain",
        talas::core::ParameterKind::Expression,
        talas::expr::ValueDomain::Scalar,
        {},
    });

    talas::project::ProjectNode component;
    component.instanceId = "component";
    component.typeId = "org.talas.activation.fixture.gain";
    component.parameters = {scalarParameter("gain", std::move(gain))};
    component.preservedComponent = capsule;

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mix", "talas.mixer", {}},
        {"input", "talas.audioInput", {}},
        std::move(component),
    };
    document.graph.connections = {
        {"input", "audioOut", "component", "in0"},
        {"input", "audioOut", "mix", "in1"},
        {"component", "out0", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument isolatedResourceDocument(
    std::string resourceId = "Tables/Curve-A")
{
    talas::project::ProjectPreservedComponent capsule;
    capsule.typeVersion = "1.0.0";
    capsule.stateSchemaVersion = 1;
    capsule.displayName = "Prepared isolated immutable resource";
    capsule.bundleId = "org.talas.activation.fixture";
    capsule.inputs = {audioPort("in0", true)};
    capsule.outputs = {audioPort("out0", true)};
    capsule.parameters.push_back({
        "table",
        talas::core::ParameterKind::Resource,
        talas::expr::ValueDomain::Scalar,
        {},
    });

    talas::project::ProjectNode component;
    component.instanceId = "component";
    component.typeId = "org.talas.activation.fixture.gain";
    component.parameters = {
        resourceParameter("table", std::move(resourceId))};
    component.preservedComponent = capsule;

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mix", "talas.mixer", {}},
        {"input", "talas.audioInput", {}},
        std::move(component),
    };
    document.graph.connections = {
        {"input", "audioOut", "component", "in0"},
        {"input", "audioOut", "mix", "in1"},
        {"component", "out0", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument isolatedAudioResourceDocument(
    std::string resourceId = "Audio/Voice-A")
{
    auto document = isolatedResourceDocument(std::move(resourceId));
    auto& node = document.graph.nodes.back();
    node.preservedComponent->displayName =
        "Prepared isolated bounded Audio stream";
    node.preservedComponent->parameters.front().parameterId = "audio";
    node.parameters.front().parameterId = "audio";
    return document;
}

talas::project::ProjectDocument isolatedResourceSetDocument()
{
    auto document = isolatedResourceDocument("Tables/Curve-A");
    auto& node = document.graph.nodes.back();
    node.preservedComponent->displayName =
        "Prepared isolated aggregate resource set";
    node.preservedComponent->parameters.push_back({
        "binary",
        talas::core::ParameterKind::Resource,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    node.preservedComponent->parameters.push_back({
        "audio",
        talas::core::ParameterKind::Resource,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    node.parameters.push_back(
        resourceParameter("binary", "Binary/Shape-B"));
    node.parameters.push_back(
        resourceParameter("audio", "Audio/Voice-A"));
    return document;
}

talas::project::ProjectDocument directDocument()
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

talas::project::ProjectDocument isolatedStereoDocument()
{
    talas::project::ProjectPreservedComponent capsule;
    capsule.typeVersion = "1.0.0";
    capsule.stateSchemaVersion = 1;
    capsule.displayName = "Prepared isolated stereo order";
    capsule.bundleId = "org.talas.activation.fixture";
    capsule.inputs = {
        audioPort("in0", true),
        audioPort("in1", true),
    };
    capsule.outputs = {
        audioPort("out0", true),
        audioPort("out1", true),
    };
    capsule.parameters.push_back({
        "gain",
        talas::core::ParameterKind::Expression,
        talas::expr::ValueDomain::Scalar,
        {},
    });

    talas::project::ProjectNode component;
    component.instanceId = "component";
    component.typeId = "org.talas.activation.fixture.stereo-order";
    component.parameters = {scalarParameter("gain", "0.5")};
    component.preservedComponent = std::move(capsule);

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"left", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }},
        {"right", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }},
        std::move(component),
    };
    document.graph.connections = {
        {"left", "audioOut", "component", "in0"},
        {"right", "audioOut", "component", "in1"},
        {"component", "out0", "out", "audioIn"},
        {"component", "out1", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings settings()
{
    talas::runtime::RenderSettings result;
    result.sampleRate = sampleRate;
    result.maxBlockFrames = blockFrames;
    return result;
}

talas::runtime::component_activation::ComponentActivationStaticParameterV1
gainParameter(double gain)
{
    using namespace talas::runtime::component_activation;
    ComponentActivationStaticParameterV1 result;
    result.kind = static_cast<std::uint32_t>(
        ComponentActivationStaticParameterKindV1::Scalar);
    result.idBytes = 4;
    std::copy_n("gain", 4, result.id.begin());
    result.scalarValue = gain;
    return result;
}

talas::runtime::component_activation::ComponentActivationStaticParameterV1
resourceStaticParameter(std::string_view resourceId = "Tables/Curve-A")
{
    using namespace talas::runtime::component_activation;
    ComponentActivationStaticParameterV1 result;
    result.kind = static_cast<std::uint32_t>(
        ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "table";
    result.idBytes = parameterId.size();
    result.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), result.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), result.text.begin());
    return result;
}

talas::runtime::component_activation::ComponentActivationStaticParameterV1
resourceSetStaticParameter(
    std::string_view parameterId,
    std::string_view resourceId)
{
    using namespace talas::runtime::component_activation;
    ComponentActivationStaticParameterV1 result;
    result.kind = static_cast<std::uint32_t>(
        ComponentActivationStaticParameterKindV1::Resource);
    result.idBytes = parameterId.size();
    result.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), result.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), result.text.begin());
    return result;
}

talas::runtime::component_activation::ComponentActivationStaticParameterV1
audioResourceStaticParameter(std::string_view resourceId = "Audio/Voice-A")
{
    using namespace talas::runtime::component_activation;
    ComponentActivationStaticParameterV1 result;
    result.kind = static_cast<std::uint32_t>(
        ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "audio";
    result.idBytes = parameterId.size();
    result.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), result.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), result.text.begin());
    return result;
}

talas::core::ResourceSnapshotPtr immutableTableSnapshot(
    std::uint64_t generation,
    std::vector<double> values,
    char hashByte)
{
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Tables/Curve-A";
    snapshot->kind = talas::core::ResourceSnapshotKind::Table;
    snapshot->generation = generation;
    snapshot->contentHash.assign(64, hashByte);
    snapshot->tableSamples = std::move(values);
    return snapshot;
}

std::string digestHex(
    const std::array<
        std::uint8_t,
        talas::runtime::componentTrustDigestBytes>& digest)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2U] = hexadecimal[digest[index] >> 4U];
        result[index * 2U + 1U] = hexadecimal[digest[index] & 0x0fU];
    }
    return result;
}

talas::runtime::component_bundle::ComponentBundleSnapshot
replacementCandidateSnapshot(
    const talas::project::ProjectDocument& document,
    std::string version)
{
    namespace bundle = talas::runtime::component_bundle;
    using namespace talas::runtime;
    const auto& projectNode = document.graph.nodes.back();
    const auto& preserved = *projectNode.preservedComponent;
    bundle::ComponentBundleManifest manifest;
    manifest.bundleId = preserved.bundleId;
    manifest.semanticVersion = std::move(version);
    manifest.minimumEngineAbiVersion = 1;
    manifest.maximumEngineAbiVersion = 1;
    manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V3;
    manifest.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V3;
    manifest.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor)
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    manifest.requestedPermissions = componentPermissionsForRoles(
        manifest.declaredRoles);
    manifest.entry.path = "Payload/component.bin";
    const std::array<std::uint8_t, 3> entryBytes{'a', 'b', 'c'};
    manifest.entry.byteCount = entryBytes.size();
    manifest.entry.sha256 = component_activation::
        calculateComponentActivationByteDigestV1(entryBytes);

    bundle::ComponentBundleNode node;
    node.typeId = projectNode.typeId;
    node.stateSchemaVersion = preserved.stateSchemaVersion;
    for (const auto& input : preserved.inputs)
        node.inputs.push_back({input.portId, input.type, input.required, input.signal});
    for (const auto& output : preserved.outputs)
        node.outputs.push_back({output.portId, output.type, output.required, output.signal});
    for (const auto& parameter : preserved.parameters)
    {
        node.parameters.push_back({
            parameter.parameterId,
            parameter.kind,
            parameter.domain,
            parameter.choices,
        });
    }
    manifest.nodes.push_back(std::move(node));
    manifest.state.currentSchemaVersion = preserved.stateSchemaVersion;
    manifest.provenance.publisher = "Talas replacement fixture";
    manifest.provenance.licenseExpression = "Apache-2.0";
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        manifest);
    require(canonical.ok(), "replacement candidate should canonicalize");

    bundle::ComponentBundleSnapshot snapshot;
    snapshot.manifest = std::move(manifest);
    snapshot.canonicalManifest = canonical.canonicalJson;
    snapshot.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        "Payload/component.bin",
        snapshot.manifest.entry.sha256,
        std::vector<std::uint8_t>(entryBytes.begin(), entryBytes.end()),
        true,
    });
    snapshot.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    require(
        bundle::validateComponentBundleSnapshotIdentity(snapshot),
        "replacement candidate should retain exact frozen identity");
    return snapshot;
}

talas::runtime::ComponentTrustEvidence replacementTrustEvidence(
    const talas::runtime::component_bundle::ComponentBundleSnapshot& snapshot,
    talas::runtime::component_bundle::ComponentOwnerAuthorizationStore& owner)
{
    namespace bundle = talas::runtime::component_bundle;
    using namespace talas::runtime;
    ComponentTrustEvidence evidence;
    evidence.candidate.bundleId = snapshot.manifest.bundleId;
    evidence.candidate.semanticVersion = snapshot.manifest.semanticVersion;
    evidence.candidate.contentDigest = snapshot.contentDigest;
    evidence.candidate.signerIdentityDigest.fill(0x44);
    evidence.candidate.requestedPermissions =
        snapshot.manifest.requestedPermissions;
    evidence.candidate.policyVersion = currentComponentTrustPolicyVersion;
    evidence.candidate.hasContentDigest = true;
    evidence.candidate.hasSignerIdentity = true;
    const auto action = owner.recordOwnerAction(
        evidence.candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        owner.generation());
    require(action.ok(), "replacement candidate should receive exact owner approval");
    evidence.ownerAuthorizationEvidence = owner.authorizationEvidence();
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = evidence.candidate.contentDigest;
    signature.signerIdentityDigest =
        evidence.candidate.signerIdentityDigest;
    signature.profileVersion = currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    evidence.artifactGeneration = 1;
    evidence.validationNonce.fill(0x55);
    evidence.hasValidationNonce = true;
    evidence.declaredRoles = snapshot.manifest.declaredRoles;
    return evidence;
}

talas::core::ResourceSnapshotPtr immutableBinarySnapshot(
    std::uint64_t generation,
    std::vector<std::uint8_t> bytes)
{
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Tables/Curve-A";
    snapshot->kind = talas::core::ResourceSnapshotKind::Binary;
    snapshot->generation = generation;
    snapshot->binaryBytes = std::move(bytes);
    snapshot->contentHash = digestHex(
        talas::runtime::component_activation::
            calculateComponentActivationByteDigestV1(
                snapshot->binaryBytes));
    return snapshot;
}

talas::core::ResourceSnapshotPtr immutableAudioSnapshot(
    std::uint64_t generation,
    char hashByte,
    std::uint32_t channels = 2,
    double sourceSampleRate = 48000.0,
    std::uint64_t sourceFrameCount = 8192)
{
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Audio/Voice-A";
    snapshot->kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot->generation = generation;
    snapshot->contentHash.assign(64, hashByte);
    snapshot->resolvedPath =
        "/source-was-frozen-and-deleted-before-runtime-staging.wav";
    snapshot->channelCount = channels;
    snapshot->sampleRate = sourceSampleRate;
    snapshot->frameCount = sourceFrameCount;
    snapshot->encoding = "pcm16-wav";
    return snapshot;
}

talas::runtime::RenderSettings resourceSettings(
    talas::core::ResourceSnapshotPtr snapshot)
{
    auto result = settings();
    result.resourceSnapshots.push_back(std::move(snapshot));
    return result;
}

class ImmediateOneBlockBinding final
    : public talas::runtime::PreparedIsolatedComponentBinding
{
public:
    ImmediateOneBlockBinding(
        talas::runtime::IsolatedComponentRuntimeContract contract,
        std::shared_ptr<std::atomic<bool>> destroyed,
        std::shared_ptr<std::atomic<bool>> callbackActive,
        std::shared_ptr<std::atomic<bool>> destroyedOnCallback,
        float multiplier = 0.5F,
        bool checkpointCapable = false)
        : contract_(std::move(contract))
        , destroyed_(std::move(destroyed))
        , callbackActive_(std::move(callbackActive))
        , destroyedOnCallback_(std::move(destroyedOnCallback))
        , multiplier_(multiplier)
        , checkpointCapable_(checkpointCapable)
    {
    }

    ~ImmediateOneBlockBinding() override
    {
        destroyedOnCallback_->store(
            callbackActive_->load(std::memory_order_acquire),
            std::memory_order_release);
        destroyed_->store(true, std::memory_order_release);
    }

    [[nodiscard]] const talas::runtime::IsolatedComponentRuntimeContract&
    contract() const noexcept override
    {
        return contract_;
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return active_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool supportsOfflineCheckpoint() const noexcept override { return checkpointCapable_; }

    [[nodiscard]] std::unique_ptr<talas::runtime::IsolatedComponentOfflineCheckpoint>
    beginOfflineCheckpoint() override
    {
        if (!checkpointCapable_ || !active()) return {};
        if (checkpointActive_) throw std::logic_error("Nested isolated checkpoint");
        class Checkpoint final : public talas::runtime::IsolatedComponentOfflineCheckpoint
        {
        public:
            explicit Checkpoint(ImmediateOneBlockBinding& owner)
                : owner_(&owner), previous_(owner.previous_), transport_(owner.previousTransport_),
                  processCalls_(owner.processCalls_.load(std::memory_order_relaxed))
            { owner.checkpointActive_ = true; }
            ~Checkpoint() override
            {
                if (!owner_) return;
                owner_->previous_ = previous_;
                owner_->previousTransport_ = transport_;
                owner_->processCalls_.store(processCalls_, std::memory_order_relaxed);
                owner_->checkpointActive_ = false;
            }
            bool commit() noexcept override
            {
                if (!owner_) return true;
                const auto failed = owner_->commitFailureMode_ == 1;
                if (owner_->commitFailureMode_ == 2)
                    owner_->quarantine(talas::runtime::IsolatedComponentQuarantineReason::Revoked);
                if (owner_->commitFailureMode_ == 3) owner_->denyStageAdmission_ = true;
                owner_->checkpointActive_ = false;
                owner_ = nullptr;
                return !failed;
            }
        private:
            ImmediateOneBlockBinding* owner_;
            std::array<float, blockFrames> previous_;
            talas::runtime::IsolatedComponentTransport transport_;
            std::size_t processCalls_;
        };
        auto checkpoint = std::make_unique<Checkpoint>(*this);
        ++checkpointCaptures_;
        return checkpoint;
    }

    void failNextCheckpointCommit(unsigned mode) noexcept { commitFailureMode_ = mode; }
    [[nodiscard]] std::size_t checkpointCaptures() const noexcept { return checkpointCaptures_; }

    [[nodiscard]] std::optional<
        talas::runtime::IsolatedComponentAudioStreamHealthSetSnapshot>
    audioStreamHealthSet() noexcept override
    {
        if (!contract_.resources.has_value())
            return std::nullopt;
        talas::runtime::IsolatedComponentAudioStreamHealthSetSnapshot result;
        const auto audioCount = std::ranges::count_if(
            contract_.resources->members,
            [](const auto& resource) {
                return std::holds_alternative<
                    talas::runtime::IsolatedComponentAudioStreamContract>(
                        resource);
            });
        if (audioCount == 0
            || static_cast<std::size_t>(audioCount) > result.streams.size())
        {
            return std::nullopt;
        }
        result.streamCount = audioCount;
        for (std::size_t index = 0; index < result.streamCount; ++index)
        {
            auto& stream = result.streams[index];
            stream.seekGeneration = 3 + index;
            stream.health.underrunEvents = 2;
            stream.health.missingFrames = 7;
            stream.health.discontinuities = 1;
            stream.health.requestRejections = 4;
            stream.health.responseQueueFull = 5;
            stream.health.staleResponseBlocks = 6;
            stream.health.decodedBlocks = 11;
            stream.health.endOfStreamFrames = 3;
            stream.health.bufferedBlocks = 2;
            stream.health.bufferCapacity = 8;
            stream.health.lowWatermark = 2;
            stream.health.highWatermark = 6;
            stream.health.pendingRequests = 1;
            stream.health.workerState =
            talas::runtime::AudioStreamWorkerState::Ready;
        }
        return result;
    }

    [[nodiscard]] bool tryBeginStage() noexcept override
    {
        stageAdmissionEntered_.store(true, std::memory_order_release);
        while (holdStageAdmission_.load(std::memory_order_acquire)
               && !releaseStageAdmission_.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        return active() && !denyStageAdmission_;
    }

    void endStage() noexcept override
    {
    }

    [[nodiscard]] talas::runtime::IsolatedComponentProcessResult process(
        std::span<const float* const> inputs,
        std::span<float* const> outputs,
        std::uint32_t frames,
        talas::runtime::IsolatedComponentTransport transport) noexcept override
    {
        if (!active() || inputs.size() != 1 || outputs.size() != 1
            || inputs.front() == nullptr || outputs.front() == nullptr
            || frames != blockFrames || transport.generation == 0)
        {
            return talas::runtime::IsolatedComponentProcessResult::Quarantined;
        }
        previousTransport_ = transport;
        const auto call = processCalls_.fetch_add(
            1, std::memory_order_relaxed);
        if (checkpointCapable_ && call == 0 && !transport.reset)
            return talas::runtime::IsolatedComponentProcessResult::Quarantined;
        if (call == 0)
        {
            std::copy_n(inputs.front(), frames, previous_.begin());
            std::fill_n(outputs.front(), frames, 0.0F);
            return talas::runtime::IsolatedComponentProcessResult::FirstBlockSilence;
        }
        for (std::size_t frame = 0; frame < frames; ++frame)
            outputs.front()[frame] = previous_[frame] * multiplier_;
        std::copy_n(inputs.front(), frames, previous_.begin());
        return talas::runtime::IsolatedComponentProcessResult::Rendered;
    }

    void quarantine(
        talas::runtime::IsolatedComponentQuarantineReason) noexcept override
    {
        active_.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::size_t processCalls() const noexcept
    {
        return processCalls_.load(std::memory_order_acquire);
    }

    void holdStageAdmission() noexcept
    {
        holdStageAdmission_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool stageAdmissionEntered() const noexcept
    {
        return stageAdmissionEntered_.load(std::memory_order_acquire);
    }

    void revokeAndReleaseStageAdmission() noexcept
    {
        active_.store(false, std::memory_order_release);
        releaseStageAdmission_.store(true, std::memory_order_release);
    }

private:
    talas::runtime::IsolatedComponentRuntimeContract contract_;
    std::shared_ptr<std::atomic<bool>> destroyed_;
    std::shared_ptr<std::atomic<bool>> callbackActive_;
    std::shared_ptr<std::atomic<bool>> destroyedOnCallback_;
    std::atomic<bool> active_{true};
    std::atomic<bool> holdStageAdmission_{false};
    std::atomic<bool> stageAdmissionEntered_{false};
    std::atomic<bool> releaseStageAdmission_{false};
    std::atomic<std::size_t> processCalls_{0};
    std::array<float, blockFrames> previous_{};
    float multiplier_ = 0.5F;
    const bool checkpointCapable_ = false;
    bool checkpointActive_ = false;
    unsigned commitFailureMode_ = 0;
    bool denyStageAdmission_ = false;
    std::size_t checkpointCaptures_ = 0;
    talas::runtime::IsolatedComponentTransport previousTransport_;
};

class StereoOrderBinding final
    : public talas::runtime::PreparedIsolatedComponentBinding
{
public:
    explicit StereoOrderBinding(
        talas::runtime::IsolatedComponentRuntimeContract contract)
        : contract_(std::move(contract))
    {
    }

    [[nodiscard]] const talas::runtime::IsolatedComponentRuntimeContract&
    contract() const noexcept override
    {
        return contract_;
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return active_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool tryBeginStage() noexcept override
    {
        return active();
    }

    void endStage() noexcept override
    {
    }

    [[nodiscard]] talas::runtime::IsolatedComponentProcessResult process(
        std::span<const float* const> inputs,
        std::span<float* const> outputs,
        std::uint32_t frames,
        talas::runtime::IsolatedComponentTransport) noexcept override
    {
        if (!active() || inputs.size() != 2 || outputs.size() != 2
            || inputs[0] == nullptr || inputs[1] == nullptr
            || outputs[0] == nullptr || outputs[1] == nullptr
            || frames != blockFrames)
        {
            return talas::runtime::IsolatedComponentProcessResult::Quarantined;
        }
        const auto call = processCalls_.fetch_add(
            1, std::memory_order_relaxed);
        if (call == 0)
        {
            std::copy_n(inputs[0], frames, previous_[0].begin());
            std::copy_n(inputs[1], frames, previous_[1].begin());
            std::fill_n(outputs[0], frames, 0.0F);
            std::fill_n(outputs[1], frames, 0.0F);
            return talas::runtime::IsolatedComponentProcessResult::
                FirstBlockSilence;
        }
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            outputs[0][frame] = previous_[0][frame] * 2.0F;
            outputs[1][frame] = previous_[1][frame] * 3.0F;
        }
        std::copy_n(inputs[0], frames, previous_[0].begin());
        std::copy_n(inputs[1], frames, previous_[1].begin());
        return talas::runtime::IsolatedComponentProcessResult::Rendered;
    }

    void quarantine(
        talas::runtime::IsolatedComponentQuarantineReason) noexcept override
    {
        active_.store(false, std::memory_order_release);
    }

    void revoke() noexcept
    {
        active_.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::size_t processCalls() const noexcept
    {
        return processCalls_.load(std::memory_order_acquire);
    }

private:
    talas::runtime::IsolatedComponentRuntimeContract contract_;
    std::atomic<bool> active_{true};
    std::atomic<std::size_t> processCalls_{0};
    std::array<std::array<float, blockFrames>, 2> previous_{};
};

talas::runtime::IsolatedComponentRuntimeContract bindingContract(
    const talas::project::ProjectDocument& document,
    double gain = 0.5)
{
    const auto& node = document.graph.nodes.back();
    const auto& capsule = *node.preservedComponent;
    talas::runtime::IsolatedComponentRuntimeContract contract;
    contract.nodeId = node.instanceId;
    contract.typeId = node.typeId;
    contract.typeVersion = capsule.typeVersion;
    contract.bundleId = capsule.bundleId;
    contract.stateSchemaVersion = capsule.stateSchemaVersion;
    contract.inputs = capsule.inputs;
    contract.outputs = capsule.outputs;
    contract.parameters = capsule.parameters;
    contract.staticParameters = {gainParameter(gain)};
    contract.sampleRate = static_cast<std::uint32_t>(sampleRate);
    contract.blockFrames = blockFrames;
    contract.activationGeneration = 7;
    return contract;
}

talas::runtime::IsolatedComponentRuntimeContract resourceBindingContract(
    const talas::project::ProjectDocument& document,
    const talas::core::ResourceSnapshot& snapshot)
{
    const auto& node = document.graph.nodes.back();
    const auto& capsule = *node.preservedComponent;
    talas::runtime::IsolatedComponentRuntimeContract contract;
    contract.nodeId = node.instanceId;
    contract.typeId = node.typeId;
    contract.typeVersion = capsule.typeVersion;
    contract.bundleId = capsule.bundleId;
    contract.stateSchemaVersion = capsule.stateSchemaVersion;
    contract.inputs = capsule.inputs;
    contract.outputs = capsule.outputs;
    contract.parameters = capsule.parameters;
    contract.staticParameters = {resourceStaticParameter(snapshot.id)};
    contract.sampleRate = static_cast<std::uint32_t>(sampleRate);
    contract.blockFrames = blockFrames;
    contract.activationGeneration = 11;

    std::vector<std::uint8_t> mediatedBytes;
    if (snapshot.kind == talas::core::ResourceSnapshotKind::Binary)
    {
        mediatedBytes = snapshot.binaryBytes;
    }
    else
    {
        mediatedBytes.resize(snapshot.tableSamples.size() * sizeof(double));
        require(
            talas::runtime::component_activation::
                    encodeComponentActivationImmutableTableV1(
                        snapshot.tableSamples, mediatedBytes)
                == talas::runtime::component_activation::
                    ComponentActivationProtocolDiagnosticV1::Valid,
            "resource binding fixture should encode its exact table");
    }
    talas::runtime::IsolatedComponentImmutableResourceContract resource;
    resource.parameterId = "table";
    resource.resourceId = snapshot.id;
    resource.kind = snapshot.kind;
    resource.generation = snapshot.generation;
    resource.sourceContentHash = snapshot.contentHash;
    resource.mediatedPayloadDigest =
        talas::runtime::component_activation::
            calculateComponentActivationByteDigestV1(mediatedBytes);
    resource.elementCount = snapshot.kind
            == talas::core::ResourceSnapshotKind::Binary
        ? snapshot.binaryBytes.size()
        : snapshot.tableSamples.size();
    resource.byteCount = mediatedBytes.size();
    talas::runtime::IsolatedComponentResourceSetContract resources;
    resources.members.emplace_back(std::move(resource));
    contract.resources = std::move(resources);
    return contract;
}

talas::runtime::IsolatedComponentRuntimeContract audioBindingContract(
    const talas::project::ProjectDocument& document,
    const talas::core::ResourceSnapshot& snapshot)
{
    const auto& node = document.graph.nodes.back();
    const auto& capsule = *node.preservedComponent;
    talas::runtime::IsolatedComponentRuntimeContract contract;
    contract.nodeId = node.instanceId;
    contract.typeId = node.typeId;
    contract.typeVersion = capsule.typeVersion;
    contract.bundleId = capsule.bundleId;
    contract.stateSchemaVersion = capsule.stateSchemaVersion;
    contract.inputs = capsule.inputs;
    contract.outputs = capsule.outputs;
    contract.parameters = capsule.parameters;
    contract.staticParameters = {audioResourceStaticParameter(snapshot.id)};
    contract.sampleRate = static_cast<std::uint32_t>(sampleRate);
    contract.blockFrames = blockFrames;
    contract.activationGeneration = 13;

    talas::runtime::IsolatedComponentAudioStreamContract audio;
    audio.parameterId = "audio";
    audio.resourceId = snapshot.id;
    audio.generation = snapshot.generation;
    audio.sourceContentHash = snapshot.contentHash;
    audio.sourceByteCount = 44U
        + snapshot.frameCount * snapshot.channelCount * sizeof(std::int16_t);
    audio.sourceFrameCount = snapshot.frameCount;
    audio.maximumSourceFrameIncrement =
        talas::runtime::component_activation::
            maximumComponentActivationAudioSourceFrameIncrementV1;
    audio.sourceSampleRate = static_cast<std::uint32_t>(snapshot.sampleRate);
    audio.streamProfileVersion = talas::runtime::component_activation::
        componentActivationAudioStreamProfileVersionV1;
    audio.sourceEncoding = static_cast<std::uint32_t>(
        talas::runtime::component_activation::
            ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
    audio.maximumReadFrames = talas::runtime::component_activation::
        maximumComponentActivationAudioReadFramesV1;
    audio.decodeBlockFrames = talas::runtime::component_activation::
        componentActivationAudioDecodeBlockFramesV1;
    audio.sourceChannelCount =
        static_cast<std::uint16_t>(snapshot.channelCount);
    audio.maximumTouchedSourceBlocks =
        talas::runtime::component_activation::
            maximumComponentActivationAudioTouchedSourceBlocksV1;
    audio.requestCapacity = talas::runtime::component_activation::
        componentActivationAudioRequestCapacityV1;
    audio.responseCapacity = talas::runtime::component_activation::
        componentActivationAudioResponseCapacityV1;
    audio.workerCacheCapacity = talas::runtime::component_activation::
        componentActivationAudioWorkerCacheCapacityV1;
    audio.lowWatermarkBlocks = talas::runtime::component_activation::
        componentActivationAudioLowWatermarkBlocksV1;
    audio.highWatermarkBlocks = talas::runtime::component_activation::
        componentActivationAudioHighWatermarkBlocksV1;
    talas::runtime::IsolatedComponentResourceSetContract resources;
    resources.members.emplace_back(std::move(audio));
    contract.resources = std::move(resources);
    return contract;
}

talas::runtime::IsolatedComponentImmutableResourceContract&
immutableResource(talas::runtime::IsolatedComponentRuntimeContract& contract)
{
    return std::get<talas::runtime::IsolatedComponentImmutableResourceContract>(
        contract.resources->members.front());
}

talas::runtime::IsolatedComponentAudioStreamContract&
audioResource(talas::runtime::IsolatedComponentRuntimeContract& contract)
{
    return std::get<talas::runtime::IsolatedComponentAudioStreamContract>(
        contract.resources->members.front());
}

std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes>
digestFromHex(std::string_view text)
{
    std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes> result{};
    require(text.size() == result.size() * 2U,
            "resource-set fixture digest must be canonical");
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        return static_cast<std::uint8_t>(value - 'a' + 10);
    };
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        result[index] = static_cast<std::uint8_t>(
            (nibble(text[index * 2U]) << 4U)
            | nibble(text[index * 2U + 1U]));
    }
    return result;
}

talas::runtime::IsolatedComponentRuntimeContract resourceSetBindingContract(
    const talas::project::ProjectDocument& document,
    const std::array<talas::core::ResourceSnapshotPtr, 3>& snapshots)
{
    using namespace talas::runtime;
    using namespace talas::runtime::component_activation;
    const auto& node = document.graph.nodes.back();
    const auto& capsule = *node.preservedComponent;
    IsolatedComponentRuntimeContract contract;
    contract.nodeId = node.instanceId;
    contract.typeId = node.typeId;
    contract.typeVersion = capsule.typeVersion;
    contract.bundleId = capsule.bundleId;
    contract.stateSchemaVersion = capsule.stateSchemaVersion;
    contract.inputs = capsule.inputs;
    contract.outputs = capsule.outputs;
    contract.parameters = capsule.parameters;
    contract.staticParameters = {
        resourceSetStaticParameter("table", snapshots[0]->id),
        resourceSetStaticParameter("binary", snapshots[1]->id),
        resourceSetStaticParameter("audio", snapshots[2]->id),
    };
    contract.sampleRate = static_cast<std::uint32_t>(sampleRate);
    contract.blockFrames = blockFrames;
    contract.activationGeneration = 31;

    auto tableContract = resourceBindingContract(document, *snapshots[0]);
    auto binaryContract = resourceBindingContract(document, *snapshots[1]);
    immutableResource(binaryContract).parameterId = "binary";
    auto audioContract = audioBindingContract(document, *snapshots[2]);

    IsolatedComponentResourceSetContract resources;
    resources.members = {
        immutableResource(tableContract),
        immutableResource(binaryContract),
        audioResource(audioContract),
    };
    resources.memberRecords.resize(resources.members.size());
    ComponentActivationResourceSetGrantV1 grant;
    grant.header = componentActivationHeaderV1(
        ComponentActivationRecordKindV1::ResourceSetGrant,
        sizeof(grant));
    grant.contentDigest.fill(0x31U);
    grant.activationGeneration = contract.activationGeneration;
    grant.profileVersion = componentActivationResourceSetProfileVersionV1;
    grant.memberCount = static_cast<std::uint16_t>(resources.members.size());
    grant.audioStreamCount = 1;
    for (std::size_t index = 0; index < resources.members.size(); ++index)
    {
        auto& member = resources.memberRecords[index];
        member.header = componentActivationHeaderV1(
            ComponentActivationRecordKindV1::ResourceSetMember,
            sizeof(member));
        member.memberGrantDigest.fill(
            static_cast<std::uint8_t>(index + 1U));
        member.memberOrdinal = static_cast<std::uint16_t>(index);
        member.memberCount = grant.memberCount;
        if (const auto* immutable =
                std::get_if<IsolatedComponentImmutableResourceContract>(
                    &resources.members[index]))
        {
            member.memberKind = static_cast<std::uint32_t>(
                immutable->kind == talas::core::ResourceSnapshotKind::Table
                    ? ComponentActivationResourceSetMemberKindV1::Table
                    : ComponentActivationResourceSetMemberKindV1::Binary);
            member.sourceContentDigest =
                digestFromHex(immutable->sourceContentHash);
            member.mediatedPayloadDigest =
                immutable->mediatedPayloadDigest;
            member.resourceGeneration = immutable->generation;
            member.sourceByteCount = immutable->byteCount;
            member.transportByteCount =
                (immutable->byteCount
                    + maximumComponentActivationResourceSetTransportPageBytesV1
                    - 1U)
                / maximumComponentActivationResourceSetTransportPageBytesV1
                * maximumComponentActivationResourceSetTransportPageBytesV1;
            member.privateMappedByteCount = member.transportByteCount;
            member.parameterIdBytes = immutable->parameterId.size();
            member.resourceIdBytes = immutable->resourceId.size();
            std::copy(
                immutable->parameterId.begin(),
                immutable->parameterId.end(),
                member.parameterId.begin());
            std::copy(
                immutable->resourceId.begin(),
                immutable->resourceId.end(),
                member.resourceId.begin());
        }
        else
        {
            const auto& audio =
                std::get<IsolatedComponentAudioStreamContract>(
                    resources.members[index]);
            member.memberKind = static_cast<std::uint32_t>(
                ComponentActivationResourceSetMemberKindV1::Audio);
            member.sourceContentDigest =
                digestFromHex(audio.sourceContentHash);
            member.mediatedPayloadDigest = member.sourceContentDigest;
            member.resourceGeneration = audio.generation;
            member.sourceByteCount = audio.sourceByteCount;
            member.transportByteCount =
                (audio.sourceByteCount
                    + maximumComponentActivationResourceSetTransportPageBytesV1
                    - 1U)
                / maximumComponentActivationResourceSetTransportPageBytesV1
                * maximumComponentActivationResourceSetTransportPageBytesV1;
            member.privateMappedByteCount = member.transportByteCount;
            member.workingByteCount =
                maximumComponentActivationAudioStreamWorkingBytesV1;
            member.parameterIdBytes = audio.parameterId.size();
            member.resourceIdBytes = audio.resourceId.size();
            std::copy(
                audio.parameterId.begin(),
                audio.parameterId.end(),
                member.parameterId.begin());
            std::copy(
                audio.resourceId.begin(),
                audio.resourceId.end(),
                member.resourceId.begin());
        }
        grant.aggregateSourceBytes += member.sourceByteCount;
        grant.aggregateTransportBytes += member.transportByteCount;
        grant.aggregatePrivateMappedBytes += member.privateMappedByteCount;
        grant.aggregateWorkingBytes += member.workingByteCount;
    }
    grant.resourceSetDigest = calculateComponentActivationResourceSetDigestV1(
        grant, resources.memberRecords);
    for (auto& member : resources.memberRecords)
        member.resourceSetDigest = grant.resourceSetDigest;
    require(
        validateComponentActivationResourceSetMembersV1(
            grant, resources.memberRecords)
            == ComponentActivationProtocolDiagnosticV1::Valid,
        "resource-set fixture should author one exact aggregate contract");
    resources.grant = grant;
    contract.resources = std::move(resources);
    return contract;
}

talas::runtime::IsolatedComponentRuntimeContract stereoBindingContract(
    const talas::project::ProjectDocument& document)
{
    const auto& node = document.graph.nodes.back();
    const auto& capsule = *node.preservedComponent;
    talas::runtime::IsolatedComponentRuntimeContract contract;
    contract.nodeId = node.instanceId;
    contract.typeId = node.typeId;
    contract.typeVersion = capsule.typeVersion;
    contract.bundleId = capsule.bundleId;
    contract.stateSchemaVersion = capsule.stateSchemaVersion;
    contract.inputs = capsule.inputs;
    contract.outputs = capsule.outputs;
    contract.parameters = capsule.parameters;
    contract.staticParameters = {gainParameter(0.5)};
    contract.sampleRate = static_cast<std::uint32_t>(sampleRate);
    contract.blockFrames = blockFrames;
    contract.activationGeneration = 9;
    return contract;
}

bool render(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const std::array<float, blockFrames>& input,
    std::array<float, blockFrames>& output,
    const std::shared_ptr<std::atomic<bool>>& callbackActive)
{
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float*, 1> outputChannels{output.data()};
    callbackActive->store(true, std::memory_order_release);
    const auto rendered = bridge.render(
        {inputChannels.data(), inputChannels.size()},
        outputChannels.data(),
        outputChannels.size(),
        output.size());
    callbackActive->store(false, std::memory_order_release);
    return rendered;
}

struct OfflineIsolatedFixture
{
    talas::runtime::PreparedRuntimeGraph graph;
    std::shared_ptr<ImmediateOneBlockBinding> binding;
};

OfflineIsolatedFixture makeOfflineIsolatedFixture(bool checkpointCapable = true)
{
    using namespace talas::runtime;
    auto document = isolatedDocument();
    const auto contract = bindingContract(document);
    document.graph.nodes[2] = {"input", "talas.noise", {}};
    document.graph.nodes.push_back({"gain", "talas.vca", {}});
    document.graph.nodes.push_back({"control", "talas.constant", {scalarParameter("value", "@Gain / @Divisor")}});
    document.graph.connections = {{"input", "audioOut", "component", "in0"},
        {"component", "out0", "gain", "audioIn"}, {"control", "value", "gain", "gain"},
        {"gain", "audioOut", "out", "audioIn"}};
    auto renderSettings = settings();
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"Gain", "Gain", talas::expr::ValueDomain::Scalar, 1.0},
        {"Divisor", "Divisor", talas::expr::ValueDomain::Scalar, 1.0}};
    renderSettings.enableRealtimeAnalysisCapture = true;
    renderSettings.realtimeAnalysisQueueCapacity = 8;
    renderSettings.realtimeAnalysisTargets.push_back({{}, {}, "gain", "audioOut", RealtimeAnalysisKind::Oscilloscope,
        RealtimeAnalysisPayloadKind::AudioBlock, 1, "isolated-rollback"});
    auto binding = std::make_shared<ImmediateOneBlockBinding>(contract,
        std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false),
        std::make_shared<std::atomic<bool>>(false), 0.5F, checkpointCapable);
    auto compiled = compileProjectWithIsolatedComponent(document, talas::core::makeBuiltinNodeRegistry(), renderSettings, binding);
    require(compiled.ok(), "offline isolated rollback fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    require(compiled.graph->preparedVariables().cells.size() == 2, "isolated fixture must retain both numeric variables");
    return {std::move(*compiled.graph), std::move(binding)};
}

std::array<talas::runtime::OfflineMidiVoiceEvent, 2> offlineIsolatedInputs()
{
    using namespace talas::runtime;
    std::array<OfflineMidiVoiceEvent, 2> events{};
    for (std::size_t index = 0; index != events.size(); ++index)
    {
        events[index].kind = OfflineMidiVoiceEventKind::VariableAssignment;
        events[index].sequence = events[index].batchId = index + 1;
    }
    events[0].frame = 2 * blockFrames; events[0].target.variableId = "Gain"; events[0].value = 0.5;
    events[1].frame = 4 * blockFrames; events[1].target.variableId = "Divisor"; events[1].value = 0.0;
    return events;
}

void testOfflineIsolatedComponentWholeCallRollback()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument sourceDocument;
    sourceDocument.graph.nodes = {{"input", "talas.noise", {}}, {"out", "talas.audioOutput", {}}};
    sourceDocument.graph.connections = {{"input", "audioOut", "out", "audioIn"}};
    const auto sourceGraph = compileProject(sourceDocument, talas::core::makeBuiltinNodeRegistry(), settings());
    require(sourceGraph.ok(), "independent isolated source oracle must compile");
    const auto source = renderOffline(*sourceGraph.graph, 4 * blockFrames);
    require(source.ok(), "independent isolated source oracle must render");
    for (const std::size_t prefixFrames : {std::size_t{0}, blockFrames})
    {
        auto fixture = makeOfflineIsolatedFixture();
        auto referenceFixture = makeOfflineIsolatedFixture();
        OfflineRenderSession candidate(fixture.graph), reference(referenceFixture.graph);
        const auto events = offlineIsolatedInputs();
        require(candidate.scheduleMidiVoiceEvents(events) && reference.scheduleMidiVoiceEvents(events),
            "offline isolated graph must admit exact-block combined input");
        const auto prefix = candidate.render(prefixFrames);
        const auto referencePrefix = reference.render(prefixFrames);
        require(prefix.ok() && referencePrefix.ok() && prefix.mono == referencePrefix.mono
            && fixture.binding->processCalls() == prefixFrames / blockFrames,
            "isolated component must commit its exact entry pipeline boundary");
        const auto captures = fixture.binding->checkpointCaptures();
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = candidate.render(5 * blockFrames - prefixFrames);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty(), "late isolated failure must return no partial audio");
            require(fixture.binding->processCalls() == prefixFrames / blockFrames,
                "failed offline call must restore isolated pipeline state");
        }
        require(fixture.binding->checkpointCaptures() == captures + 2,
            "root and operation binding references must capture one participant per public call");
        std::array<double, blockFrames> beforeValues{}, referenceValues{};
        const auto before = fixture.graph.analysisCapture()->copyLatestRecordResult(0, beforeValues);
        const auto referenceBefore = referenceFixture.graph.analysisCapture()->copyLatestRecordResult(0, referenceValues);
        require(before.status == referenceBefore.status && before.header == referenceBefore.header && beforeValues == referenceValues,
            "failed isolated calls must preserve previously committed analysis");
        const auto retry = candidate.render(4 * blockFrames - prefixFrames);
        const auto expected = reference.render(4 * blockFrames - prefixFrames);
        require(retry.ok() && expected.ok() && retry.mono == expected.mono,
            "isolated retry must restore queued input, pending pipeline response and native DSP phase");
        for (std::size_t index = 0; index != retry.mono.size(); ++index)
        {
            const auto frame = prefixFrames + index;
            const auto delayed = frame < blockFrames ? 0.0
                : static_cast<double>(static_cast<float>(source.mono[frame - blockFrames]) * 0.5F);
            const auto gain = frame < 2 * blockFrames ? 1.0 : 0.5;
            require(std::abs(retry.mono[index] - delayed * gain) <= 1.0e-12,
                "isolated retry must match independent source, one-block float pipeline and exact gain timing");
        }
        require(candidate.render(0).ok() && !candidate.render(blockFrames).ok()
            && fixture.binding->processCalls() == 4, "isolated invalid input must remain at its original boundary");
    }
}

void testOfflineIsolatedParticipantFailureAndRetirementBoundaries()
{
    using namespace talas::runtime;
    const auto events = offlineIsolatedInputs();
    auto legacy = makeOfflineIsolatedFixture(false);
    require(!legacy.binding->supportsOfflineCheckpoint() && !legacy.binding->beginOfflineCheckpoint(),
        "existing bindings must not claim an unimplemented checkpoint participant");
    OfflineRenderSession legacySession(legacy.graph);
    require(legacySession.scheduleMidiVoiceEvents(events) && legacySession.render(blockFrames).ok(),
        "absent checkpoint capability must preserve existing successful execution admission");

    auto fixture = makeOfflineIsolatedFixture();
    OfflineRenderSession candidate(fixture.graph);
    require(candidate.scheduleMidiVoiceEvents(events), "busy participant fixture must admit input before capture");
    auto held = fixture.binding->beginOfflineCheckpoint();
    const auto busy = candidate.render(blockFrames);
    require(held && !busy.ok() && busy.mono.empty() && fixture.binding->processCalls() == 0,
        "nested participant capture must reject before processing without replacing its baseline");
    offlineAllocationCount = 0; countOfflineAllocations = true;
    held.reset();
    countOfflineAllocations = false;
    require(offlineAllocationCount == 0, "participant restore must allocate nothing");
    require(candidate.render(blockFrames).ok() && fixture.binding->processCalls() == 1,
        "a failed nested capture must leave the original entry retryable");
    auto revoked = fixture.binding->beginOfflineCheckpoint();
    fixture.binding->quarantine(IsolatedComponentQuarantineReason::Revoked);
    revoked.reset();
    require(!fixture.binding->active() && fixture.binding->supportsOfflineCheckpoint(),
        "processing rollback must preserve revocation and immutable checkpoint capability");
    const auto inactive = candidate.render(blockFrames);
    require(!inactive.ok() && inactive.mono.empty() && fixture.binding->processCalls() == 1,
        "unavailable supporting participant must reject capture without fallback processing or reactivation");
}

void testOfflineIsolatedCommitFailureIsTerminal()
{
    using namespace talas::runtime;
    for (unsigned mode : {1U, 2U, 3U})
    for (const bool consumePrefix : {false, true})
    {
        auto fixture = makeOfflineIsolatedFixture();
        OfflineRenderSession candidate(fixture.graph);
        require(candidate.scheduleMidiVoiceEvents(offlineIsolatedInputs())
            && candidate.render(blockFrames).ok(), "terminal commit fixture must publish an initial prefix");
        std::array<double, blockFrames> beforeValues{}, afterValues{};
        const auto capture = fixture.graph.analysisCapture();
        const auto before = capture->copyLatestRecordResult(0,
            consumePrefix ? std::span<double>{beforeValues} : std::span<double>{});
        require(before.status == (consumePrefix ? RealtimeAnalysisCopyStatus::Accepted
            : RealtimeAnalysisCopyStatus::DestinationTooSmall), "prefix analysis must remain available before terminal commit");
        const auto dropped = capture->droppedBlockCount(0);
        const auto rejected = capture->rejectedRecordCount(0);
        fixture.binding->failNextCheckpointCommit(mode);
        const auto failed = candidate.render(2 * blockFrames);
        require(!failed.ok() && failed.mono.empty() && failed.right.empty()
            && failed.channels.empty() && failed.midiEvents.empty() && failed.globalControllerEvents.empty(),
            "terminal isolated commit failure must reject the entire assembled offline result");
        const auto after = capture->copyLatestRecordResult(0, afterValues);
        require((consumePrefix ? after.status == RealtimeAnalysisCopyStatus::NoRecord
            : after.status == RealtimeAnalysisCopyStatus::Accepted && after.header == before.header
                && std::ranges::all_of(afterValues, [](double value) { return value == 0.0; }))
            && capture->droppedBlockCount(0) == dropped && capture->rejectedRecordCount(0) == rejected,
            "terminal commit preserves unread prefix analysis without publishing provisional records or counters");
        require(!fixture.binding->active(), "failed participant commit must retire audio authority");
        const auto calls = fixture.binding->processCalls();
        require(!candidate.render(blockFrames).ok() && fixture.binding->processCalls() == calls,
            "a partly committed terminal participant must never resume as a successful retry");
    }
}

void testOfflineIsolatedCommitAndTerminalCleanupAllocateNothing()
{
    using namespace talas::runtime;
    for (unsigned mode : {0U, 1U, 2U, 3U})
    {
        auto fixture = makeOfflineIsolatedFixture();
        RealtimeRenderSession session(fixture.graph);
        auto transaction = RealtimeRenderSessionTestAccess::beginOfflineTransaction(session);
        require(bool(transaction), "isolated commit allocation fixture must capture all participants");
        std::array<double, blockFrames> output{};
        require(session.render(output), "isolated commit fixture must render before finalization");
        transaction->collectAnalysis(blockFrames);
        fixture.binding->failNextCheckpointCommit(mode);
        offlineAllocationCount = 0;
        countOfflineAllocations = true;
        const auto committed = transaction->commit();
        const auto repeated = transaction->commit();
        transaction.reset();
        countOfflineAllocations = false;
        require(committed == (mode == 0) && repeated == committed && offlineAllocationCount == 0,
            "successful or terminal commit and repeated finalization/destruction must allocate nothing");
    }
}

void testOfflineIsolatedAllocationFailureIsAtomic()
{
    using namespace talas::runtime;
    const auto events = offlineIsolatedInputs();
    auto referenceFixture = makeOfflineIsolatedFixture();
    OfflineRenderSession reference(referenceFixture.graph);
    require(reference.scheduleMidiVoiceEvents(events) && reference.render(blockFrames).ok(), "isolated allocation oracle must commit prefix");
    std::array<double, blockFrames> beforeValues{};
    const auto before = referenceFixture.graph.analysisCapture()->copyLatestRecordResult(0, beforeValues);
    offlineAllocationCount = 0; countOfflineAllocations = true;
    const auto expected = reference.render(3 * blockFrames);
    countOfflineAllocations = false;
    const auto sites = offlineAllocationCount;
    require(expected.ok() && sites > 20, "isolated allocation sweep must include native capture, participant and output assembly");
    for (std::size_t index = 0; index != sites; ++index)
    {
        auto fixture = makeOfflineIsolatedFixture();
        OfflineRenderSession candidate(fixture.graph);
        require(candidate.scheduleMidiVoiceEvents(events) && candidate.render(blockFrames).ok(), "isolated allocation candidate must commit prefix");
        offlineFaultCountdown = index; offlineFaultTriggered = false;
        const auto failed = candidate.render(3 * blockFrames);
        offlineFaultCountdown = std::numeric_limits<std::size_t>::max();
        require(offlineFaultTriggered && !failed.ok() && failed.mono.empty() && failed.channels.empty()
            && fixture.binding->processCalls() == 1, "isolated allocation failure must preserve complete entry state: " + std::to_string(index));
        std::array<double, blockFrames> afterValues{};
        const auto after = fixture.graph.analysisCapture()->copyLatestRecordResult(0, afterValues);
        require(before.status == after.status && before.header == after.header && beforeValues == afterValues,
            "isolated allocation failure must preserve prior analysis publications");
        const auto retry = candidate.render(3 * blockFrames);
        require(retry.ok() && retry.mono == expected.mono && fixture.binding->processCalls() == 4,
            "isolated allocation retry must restore pending response, native phase and variable schedule");
    }
    std::cout << "P2 isolated participant whole-call allocation failure/retry points: " << sites << '\n';
}

void testBinaryResourceBindingStagesExactRawIdentity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedResourceDocument();
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    const auto makeBinding = [&](
                                 talas::runtime::IsolatedComponentRuntimeContract
                                     contract,
                                 float multiplier = 0.5F) {
        return std::make_shared<ImmediateOneBlockBinding>(
            std::move(contract),
            std::make_shared<std::atomic<bool>>(false),
            callbackActive,
            std::make_shared<std::atomic<bool>>(false),
            multiplier);
    };
    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "a direct graph should stage before Binary admission tests");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "the prior direct graph should be audible before Binary staging");

    const auto generationOne = immutableBinarySnapshot(
        1, {0x00, 0x7f, 0x80, 0xfe, 0xff});
    const auto exactGenerationOne = resourceBindingContract(
        document, *generationOne);
    const auto expectRejectedPreservingDirect = [&](
                                                    auto contract,
                                                    talas::runtime::RenderSettings
                                                        candidateSettings,
                                                    std::string_view message) {
        require(
            bridge.compileAndStageIsolatedComponent(
                      document,
                      std::move(candidateSettings),
                      makeBinding(std::move(contract)))
                    .outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed,
            std::string(message));
        require(
            render(bridge, input, output, callbackActive)
                && std::ranges::all_of(output, [](float value) {
                       return value == 2.0F;
                   }),
            "a Binary identity mismatch must preserve the prior direct graph");
    };

    expectRejectedPreservingDirect(
        exactGenerationOne,
        settings(),
        "a Binary contract without its exact snapshot must reject");
    auto duplicateSettings = resourceSettings(generationOne);
    duplicateSettings.resourceSnapshots.push_back(generationOne);
    expectRejectedPreservingDirect(
        exactGenerationOne,
        std::move(duplicateSettings),
        "duplicate Binary snapshots must reject");
    auto wrongKind = exactGenerationOne;
    immutableResource(wrongKind).kind = talas::core::ResourceSnapshotKind::Table;
    expectRejectedPreservingDirect(
        std::move(wrongKind),
        resourceSettings(generationOne),
        "a Binary snapshot with a Table contract must reject");
    auto wrongSourceSnapshot = std::make_shared<talas::core::ResourceSnapshot>(
        *generationOne);
    wrongSourceSnapshot->contentHash.assign(64, 'a');
    auto wrongSourceContract = exactGenerationOne;
    immutableResource(wrongSourceContract).sourceContentHash =
        wrongSourceSnapshot->contentHash;
    expectRejectedPreservingDirect(
        std::move(wrongSourceContract),
        resourceSettings(wrongSourceSnapshot),
        "Binary source identity must equal its mediated raw-byte digest");
    auto wrongMediatedDigest = exactGenerationOne;
    immutableResource(wrongMediatedDigest).mediatedPayloadDigest.front()
        ^= 0x01U;
    expectRejectedPreservingDirect(
        std::move(wrongMediatedDigest),
        resourceSettings(generationOne),
        "Binary mediated digest drift must reject");
    auto wrongElementCount = exactGenerationOne;
    ++immutableResource(wrongElementCount).elementCount;
    expectRejectedPreservingDirect(
        std::move(wrongElementCount),
        resourceSettings(generationOne),
        "Binary element-count drift must reject");
    auto wrongByteCount = exactGenerationOne;
    ++immutableResource(wrongByteCount).byteCount;
    expectRejectedPreservingDirect(
        std::move(wrongByteCount),
        resourceSettings(generationOne),
        "Binary byte-count drift must reject");
    auto mutatedBytes = std::make_shared<talas::core::ResourceSnapshot>(
        *generationOne);
    mutatedBytes->binaryBytes[2] ^= 0xffU;
    expectRejectedPreservingDirect(
        exactGenerationOne,
        resourceSettings(mutatedBytes),
        "same-sized Binary byte mutation must reject by recomputed digest");
    auto contradictoryShape = std::make_shared<talas::core::ResourceSnapshot>(
        *generationOne);
    contradictoryShape->tableSamples = {0.5};
    expectRejectedPreservingDirect(
        exactGenerationOne,
        resourceSettings(contradictoryShape),
        "Binary snapshots with Table storage must reject");
    const auto empty = immutableBinarySnapshot(1, {});
    expectRejectedPreservingDirect(
        resourceBindingContract(document, *empty),
        resourceSettings(empty),
        "empty Binary snapshots must reject");
    const auto oversized = immutableBinarySnapshot(
        1,
        std::vector<std::uint8_t>(
            talas::runtime::component_activation::
                    maximumComponentActivationImmutableBinaryBytesV1
                + 1U,
            0x55));
    expectRejectedPreservingDirect(
        resourceBindingContract(document, *oversized),
        resourceSettings(oversized),
        "oversized Binary snapshots must reject before graph publication");

    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationOne),
                  makeBinding(exactGenerationOne))
                .ok(),
        "one exact immutable Binary contract should stage");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 0.0F;
               })
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.0F) < 1.0e-6F;
               }),
        "Binary generation one should become audible only after callback adoption");
    const auto acceptedOne = bridge.acceptedResourceIdentity(
        generationOne->id);
    require(
        acceptedOne.has_value()
            && acceptedOne->kind == talas::core::ResourceSnapshotKind::Binary
            && acceptedOne->generation == 1
            && acceptedOne->contentHash == generationOne->contentHash,
        "only the exact Binary identity should enter accepted history");

    const auto sameGenerationDrift = immutableBinarySnapshot(
        1, {0x10, 0x20, 0x30});
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(sameGenerationDrift),
                  makeBinding(resourceBindingContract(
                      document, *sameGenerationDrift)))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "same-generation Binary content drift must reject before publication");

    const auto generationTwo = immutableBinarySnapshot(
        2, {0x10, 0x20, 0x30});
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationTwo),
                  makeBinding(
                      resourceBindingContract(document, *generationTwo),
                      0.75F))
                .ok(),
        "a higher-generation Binary replacement should stage");
    require(
        render(bridge, input, output, callbackActive)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.5F) < 1.0e-6F;
               }),
        "Binary-to-Binary replacement should adopt only at a callback boundary");
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationOne),
                  makeBinding(exactGenerationOne))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Binary generation rollback must reject");

    const auto generationThreeBinary = immutableBinarySnapshot(
        3, {0x40, 0x50, 0x60});
    auto detachedReplacement = resourceBindingContract(
        document, *generationThreeBinary);
    immutableResource(detachedReplacement).mediatedPayloadDigest.front()
        ^= 0x01U;
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationThreeBinary),
                  makeBinding(std::move(detachedReplacement), 1.0F))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "a detached Binary replacement must reject before publication");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(generationTwo->id)->generation
                == 2,
        "failed Binary replacement must preserve prior output and history");

    auto revokedReplacement = makeBinding(
        resourceBindingContract(document, *generationThreeBinary), 1.0F);
    revokedReplacement->holdStageAdmission();
    std::thread revokeThread([revokedReplacement] {
        while (!revokedReplacement->stageAdmissionEntered())
            std::this_thread::yield();
        revokedReplacement->revokeAndReleaseStageAdmission();
    });
    const auto revokedStage = bridge.compileAndStageIsolatedComponent(
        document,
        resourceSettings(generationThreeBinary),
        revokedReplacement);
    revokeThread.join();
    require(
        revokedStage.outcome == talas::runtime::ProjectStageOutcome::EngineRejected
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(generationTwo->id)->generation
                == 2,
        "Binary revocation before publication must preserve output and history");

    const auto generationThreeTable = immutableTableSnapshot(
        3, {0.5, 0.75, 1.0}, 'c');
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationThreeTable),
                  makeBinding(
                      resourceBindingContract(document, *generationThreeTable),
                      1.0F))
                .ok(),
        "a higher-generation Binary-to-Table replacement should stage");
    require(
        render(bridge, input, output, callbackActive)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.0F) < 1.0e-6F;
               }),
        "Binary-to-Table replacement should adopt at a callback boundary");
    const auto acceptedTable = bridge.acceptedResourceIdentity(
        generationThreeTable->id);
    require(
        acceptedTable.has_value()
            && acceptedTable->kind == talas::core::ResourceSnapshotKind::Table
            && acceptedTable->generation == 3
            && acceptedTable->contentHash == generationThreeTable->contentHash,
        "successful cross-kind replacement should advance history exactly once");
}

void testAudioStreamBindingIsExactPathlessAndFailureAtomic()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedAudioResourceDocument();
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    const auto makeBinding = [&](
                                 talas::runtime::IsolatedComponentRuntimeContract
                                     contract,
                                 float multiplier = 0.5F) {
        return std::make_shared<ImmediateOneBlockBinding>(
            std::move(contract),
            std::make_shared<std::atomic<bool>>(false),
            callbackActive,
            std::make_shared<std::atomic<bool>>(false),
            multiplier);
    };

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "a direct graph should stage before Audio stream admission tests");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "the prior direct graph should be audible before Audio staging");

    const auto generationOne = immutableAudioSnapshot(1, 'a');
    const auto exactGenerationOne = audioBindingContract(
        document, *generationOne);
    const auto expectRejectedPreservingDirect = [&](
                                                    auto contract,
                                                    talas::runtime::RenderSettings
                                                        candidateSettings,
                                                    std::string_view message) {
        require(
            bridge.compileAndStageIsolatedComponent(
                      document,
                      std::move(candidateSettings),
                      makeBinding(std::move(contract)))
                    .outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed,
            std::string(message));
        require(
            render(bridge, input, output, callbackActive)
                && std::ranges::all_of(output, [](float value) {
                       return value == 2.0F;
                   }),
            "an Audio contract mismatch must preserve the prior direct graph");
    };

    expectRejectedPreservingDirect(
        exactGenerationOne,
        settings(),
        "an Audio contract without its exact snapshot must reject");
    auto missingSummary = exactGenerationOne;
    missingSummary.resources.reset();
    expectRejectedPreservingDirect(
        std::move(missingSummary),
        resourceSettings(generationOne),
        "an Audio parameter without host-authored metadata must reject");
    auto contradictorySummary = exactGenerationOne;
    contradictorySummary.resources->members.emplace_back(
        talas::runtime::IsolatedComponentImmutableResourceContract{});
    expectRejectedPreservingDirect(
        std::move(contradictorySummary),
        resourceSettings(generationOne),
        "Audio and immutable resource metadata must be mutually exclusive");
    auto duplicateSettings = resourceSettings(generationOne);
    duplicateSettings.resourceSnapshots.push_back(generationOne);
    expectRejectedPreservingDirect(
        exactGenerationOne,
        std::move(duplicateSettings),
        "duplicate Audio snapshots must reject");

    const auto expectMetadataMismatch = [&](auto mutate, std::string_view message) {
        auto contract = exactGenerationOne;
        mutate(audioResource(contract));
        expectRejectedPreservingDirect(
            std::move(contract), resourceSettings(generationOne), message);
    };
    expectMetadataMismatch(
        [](auto& audio) { audio.resourceId = "Audio/Other"; },
        "a detached Audio resource ID must reject");
    expectMetadataMismatch(
        [](auto& audio) { ++audio.generation; },
        "Audio generation drift must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.sourceContentHash.assign(64, 'b'); },
        "Audio content-hash drift must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.sourceChannelCount = 1; },
        "Audio channel-count drift must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.sourceSampleRate = 44100; },
        "Audio sample-rate drift must reject");
    expectMetadataMismatch(
        [](auto& audio) { --audio.sourceFrameCount; },
        "Audio frame-count drift must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.sourceEncoding = 0; },
        "an unknown Audio encoding must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.streamProfileVersion = 0; },
        "an unknown Audio stream profile must reject");
    expectMetadataMismatch(
        [](auto& audio) { audio.sourceByteCount = 0; },
        "an empty Audio source contract must reject");
    expectMetadataMismatch(
        [](auto& audio) { --audio.maximumReadFrames; },
        "Audio stream-capability drift must reject");

    auto sharedSourceDocument = document;
    talas::project::ProjectNode diskPlayer;
    diskPlayer.instanceId = "disk";
    diskPlayer.typeId = "talas.diskPlayer";
    diskPlayer.parameters = {
        resourceParameter("fileName", generationOne->id),
        scalarParameter("rateScale", "1.0"),
        scalarParameter("filePosition", "0 s"),
        scalarParameter("duration", "1 s"),
    };
    sharedSourceDocument.graph.nodes.push_back(std::move(diskPlayer));
    sharedSourceDocument.graph.connections.push_back(
        {"disk", "audioOut", "mix", "in3"});
    require(
        bridge.compileAndStageIsolatedComponent(
                  sharedSourceDocument,
                  resourceSettings(generationOne),
                  makeBinding(exactGenerationOne))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "the private isolated Audio source must not be shared with a native runtime node");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "rejected Audio source sharing must preserve the prior direct graph");

    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationOne),
                  makeBinding(exactGenerationOne, 0.25F))
                .ok(),
        "one exact Audio contract should stage without reopening its deleted path");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 0.0F;
               })
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               }),
        "the exact Audio graph should become audible after callback adoption");
    const auto acceptedOne = bridge.acceptedResourceIdentity(generationOne->id);
    const auto health = bridge.streamHealth();
    require(
        acceptedOne.has_value()
            && acceptedOne->kind == talas::core::ResourceSnapshotKind::Audio
            && acceptedOne->generation == 1
            && acceptedOne->contentHash == std::string(64, 'a'),
        "only the successfully staged Audio identity should enter history");
    require(
        health.consumerCount == 1 && health.consumers.size() == 1
            && health.consumers.front().nodeId == "component"
            && health.consumers.front().structuralSourcePath.empty()
            && health.consumers.front().resourceId == generationOne->id
            && health.consumers.front().seekGeneration == 3
            && health.totals.underrunEvents == 2
            && health.totals.missingFrames == 7
            && health.totals.discontinuities == 1
            && health.totals.requestRejections == 4
            && health.totals.responseQueueFull == 5
            && health.totals.staleResponseBlocks == 6
            && health.totals.decodedBlocks == 11
            && health.totals.endOfStreamFrames == 3,
        "the graph health aggregate should expose the exact pathless isolated Audio stream snapshot");

    const auto generationTwo = immutableAudioSnapshot(2, 'b');
    auto detachedReplacement = audioBindingContract(document, *generationTwo);
    --audioResource(detachedReplacement).sourceFrameCount;
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationTwo),
                  makeBinding(std::move(detachedReplacement), 0.75F))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "a detached Audio replacement must reject before publication");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(generationOne->id)->generation
                == 1,
        "a failed Audio replacement must preserve output and history");

    auto revokedReplacement = makeBinding(
        audioBindingContract(document, *generationTwo), 0.75F);
    revokedReplacement->holdStageAdmission();
    std::thread revokeThread([revokedReplacement] {
        while (!revokedReplacement->stageAdmissionEntered())
            std::this_thread::yield();
        revokedReplacement->revokeAndReleaseStageAdmission();
    });
    const auto revokedStage = bridge.compileAndStageIsolatedComponent(
        document, resourceSettings(generationTwo), revokedReplacement);
    revokeThread.join();
    require(
        revokedStage.outcome == talas::runtime::ProjectStageOutcome::EngineRejected
            && bridge.acceptedResourceIdentity(generationOne->id)->generation
                == 1
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               }),
        "Audio revocation before publication must preserve output and history");

    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationTwo),
                  makeBinding(
                      audioBindingContract(document, *generationTwo), 0.75F))
                .ok(),
        "a strictly newer exact Audio generation should stage");
    require(
        bridge.acceptedResourceIdentity(generationOne->id)->generation == 2,
        "successful Audio replacement should advance history exactly once");

    auto sameGenerationDrift = immutableAudioSnapshot(2, 'c');
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(sameGenerationDrift),
                  makeBinding(audioBindingContract(
                      document, *sameGenerationDrift)))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "same-generation Audio content drift must reject in history");

    auto tableAfterAudioValue = std::make_shared<talas::core::ResourceSnapshot>();
    tableAfterAudioValue->id = generationOne->id;
    tableAfterAudioValue->kind = talas::core::ResourceSnapshotKind::Table;
    tableAfterAudioValue->generation = 3;
    tableAfterAudioValue->contentHash.assign(64, 'd');
    tableAfterAudioValue->tableSamples = {0.25, 0.5};
    talas::core::ResourceSnapshotPtr tableAfterAudio = tableAfterAudioValue;
    require(
        bridge.compileAndStageIsolatedComponent(
                  isolatedResourceDocument(generationOne->id),
                  resourceSettings(tableAfterAudio),
                  makeBinding(bindingContract(isolatedDocument())))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Audio-to-Table history transitions must remain closed");
}

void testAudioStreamHealthNeverDropsAcrossInitialGraphAdoption()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedAudioResourceDocument();
    const auto source = immutableAudioSnapshot(1, 'a');
    const auto contract = audioBindingContract(document, *source);
    for (std::size_t iteration = 0; iteration < 256U; ++iteration)
    {
        auto callbackActive = std::make_shared<std::atomic<bool>>(false);
        auto binding = std::make_shared<ImmediateOneBlockBinding>(
            contract,
            std::make_shared<std::atomic<bool>>(false),
            callbackActive,
            std::make_shared<std::atomic<bool>>(false),
            0.5F);
        talas::runtime::ProjectRuntimeBridge bridge(document, registry);
        bridge.setPlaying(true);
        require(
            bridge.compileAndStageIsolatedComponent(
                      document, resourceSettings(source), std::move(binding))
                    .ok(),
            "health-adoption fixture should stage one pending Audio graph");

        std::atomic<bool> stop{false};
        std::atomic<bool> missed{false};
        std::atomic<std::uint64_t> observations{0};
        std::thread reader([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                if (bridge.streamHealth().consumerCount != 1U)
                    missed.store(true, std::memory_order_release);
                observations.fetch_add(1U, std::memory_order_release);
            }
        });
        while (observations.load(std::memory_order_acquire) < 4U)
            std::this_thread::yield();
        const auto beforeAdoption =
            observations.load(std::memory_order_acquire);
        std::array<float, blockFrames> input{};
        std::array<float, blockFrames> output{};
        require(
            render(bridge, input, output, callbackActive),
            "health-adoption fixture should render its first callback");
        while (observations.load(std::memory_order_acquire)
            < beforeAdoption + 4U)
        {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
        reader.join();
        require(
            !missed.load(std::memory_order_acquire)
                && bridge.streamHealth().consumerCount == 1U,
            "a coherent graph snapshot must preserve Audio health across pending-to-active adoption");
    }
}

void testTransientBindingStagesWithoutMutatingPreservedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedDocument();
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    require(
        registry.findNodeType("org.talas.activation.fixture.gain")
            == nullptr,
        "fixture component must begin outside the canonical registry");

    talas::runtime::ProjectRuntimeBridge ordinary(document, registry);
    require(
        ordinary.compileAndStage(settings()).outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "ordinary Open/Play compilation must keep the capsule preserve-only");

    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    auto destroyedOnCallback = std::make_shared<std::atomic<bool>>(false);
    auto binding = std::make_shared<ImmediateOneBlockBinding>(
        bindingContract(document),
        destroyed,
        callbackActive,
        destroyedOnCallback);

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "a prior audible graph should stage before isolated replacement");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(render(bridge, input, output, callbackActive),
            "the prior graph should activate");
    require(std::ranges::all_of(output, [](float value) {
                return value == 2.0F;
            }),
            "the prior graph should be exactly audible");

    auto mismatched = std::make_shared<ImmediateOneBlockBinding>(
        bindingContract(document, 0.75),
        std::make_shared<std::atomic<bool>>(false),
        callbackActive,
        std::make_shared<std::atomic<bool>>(false));
    require(
        bridge.compileAndStageIsolatedComponent(
                  document, settings(), mismatched)
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "a binding prepared with different static parameters must fail before staging");
    require(render(bridge, input, output, callbackActive),
            "failed isolated preparation must preserve the active graph");
    require(std::ranges::all_of(output, [](float value) {
                return value == 2.0F;
            }),
            "failed isolated preparation must preserve prior audible bytes");

    auto loopedDocument = document;
    loopedDocument.timeline.loop.enabled = true;
    loopedDocument.timeline.loop.startSeconds = 0.0;
    loopedDocument.timeline.loop.endSeconds = 1.0;
    require(
        bridge.compileAndStageIsolatedComponent(
                  loopedDocument, settings(), binding)
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "a transport loop that could segment an exact callback must fail before staging");
    require(
        binding->processCalls() == 0
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "segmenting-profile rejection must not enter the component or replace the audible graph");

    const auto staged = bridge.compileAndStageIsolatedComponent(
        document, settings(), binding);
    require(
        staged.ok()
            && staged.stageResult
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "one exact prepared isolated binding should stage");
    binding.reset();
    require(render(bridge, input, output, callbackActive),
            "the isolated candidate should activate at the next callback boundary");
    require(std::ranges::all_of(output, [](float value) {
                return value == 0.0F;
            }),
            "the first isolated callback and compensated parallel path must be silent");
    require(render(bridge, input, output, callbackActive),
            "the second isolated callback should render");
    require(std::ranges::all_of(output, [](float value) {
                return std::abs(value - 3.0F) < 1.0e-6F;
            }),
            "one-block component output and delayed built-in path must align exactly");

    require(
        registry.findNodeType("org.talas.activation.fixture.gain")
                == nullptr
            && talas::project::serializeProjectDocument(document)
                == canonicalBefore
            && talas::project::serializeProjectDocument(
                   bridge.editor().document())
                == talas::project::serializeProjectDocument(directDocument()),
        "transient resolution must not mutate registry, capsule, or editor document");

    require(
        bridge.compileAndStageDocument(directDocument(), settings()).ok(),
        "a built-in replacement should stage after isolated activation");
    require(render(bridge, input, output, callbackActive),
            "the built-in replacement should activate");
    require(!destroyed->load(std::memory_order_acquire),
            "the audio callback must only retire, never destroy, the isolated binding");
    require(bridge.collectRetired(),
            "the control thread should collect the isolated engine");
    require(
        destroyed->load(std::memory_order_acquire)
            && !destroyedOnCallback->load(std::memory_order_acquire),
        "binding destruction must occur only during off-callback reclamation");
}

void testTransientBindingRetainsStereoPortOrder()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedStereoDocument();
    auto binding = std::make_shared<StereoOrderBinding>(
        stereoBindingContract(document));
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStageIsolatedComponent(
                  document, settings(), binding)
                .ok(),
        "the exact stereo binding should stage");

    std::array<float, blockFrames> leftInput{};
    std::array<float, blockFrames> rightInput{};
    std::array<float, blockFrames> leftOutput{};
    std::array<float, blockFrames> rightOutput{};
    leftInput.fill(1.0F);
    rightInput.fill(10.0F);
    const std::array<const float*, 2> inputs{
        leftInput.data(), rightInput.data()};
    std::array<float*, 2> outputs{
        leftOutput.data(), rightOutput.data()};
    const talas::runtime::RealtimeAudioInputBlock inputBlock{
        inputs.data(), inputs.size()};
    require(
        bridge.render(
            inputBlock,
            outputs.data(),
            outputs.size(),
            blockFrames)
            && std::ranges::all_of(leftOutput, [](float value) {
                   return value == 0.0F;
               })
            && std::ranges::all_of(rightOutput, [](float value) {
                   return value == 0.0F;
               }),
        "the first stereo callback should be silent on both ordered ports");
    require(
        bridge.render(
            inputBlock,
            outputs.data(),
            outputs.size(),
            blockFrames)
            && std::ranges::all_of(leftOutput, [](float value) {
                   return value == 2.0F;
               })
            && std::ranges::all_of(rightOutput, [](float value) {
                   return value == 30.0F;
               }),
        "stereo input and output ports must retain exact manifest order");
    const auto callsBeforeRevocation = binding->processCalls();
    binding->revoke();
    leftOutput.fill(9.0F);
    rightOutput.fill(9.0F);
    require(
        bridge.render(
            inputBlock,
            outputs.data(),
            outputs.size(),
            blockFrames)
            && binding->processCalls() == callsBeforeRevocation
            && std::ranges::all_of(leftOutput, [](float value) {
                   return value == 0.0F;
               })
            && std::ranges::all_of(rightOutput, [](float value) {
                   return value == 0.0F;
               }),
        "revocation must silence the next callback without another component call");
}

void testImmutableResourceBindingIsExactAndReplacementIsFailureAtomic()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedResourceDocument();
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    const auto makeBinding = [&](
                                 talas::runtime::IsolatedComponentRuntimeContract
                                     contract,
                                 float multiplier,
                                 std::shared_ptr<std::atomic<bool>> destroyed =
                                     std::make_shared<std::atomic<bool>>(false),
                                 std::shared_ptr<std::atomic<bool>>
                                     destroyedOnCallback =
                                         std::make_shared<std::atomic<bool>>(
                                             false)) {
        return std::make_shared<ImmediateOneBlockBinding>(
            std::move(contract),
            std::move(destroyed),
            callbackActive,
            std::move(destroyedOnCallback),
            multiplier);
    };

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "a direct graph should stage before immutable-resource admission tests");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "the prior direct graph should be audible before resource staging");

    const auto generationOne = immutableTableSnapshot(
        1, {0.25, 0.5, 0.75}, 'a');
    const auto exactGenerationOne = resourceBindingContract(
        document, *generationOne);
    const auto expectRejectedPreservingDirect = [&](
                                                    auto contract,
                                                    talas::runtime::RenderSettings
                                                        candidateSettings,
                                                    std::string_view message) {
        const auto candidate = makeBinding(
            std::move(contract), 0.25F);
        require(
            bridge.compileAndStageIsolatedComponent(
                      document,
                      std::move(candidateSettings),
                      candidate)
                    .outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed,
            std::string(message));
        require(
            render(bridge, input, output, callbackActive)
                && std::ranges::all_of(output, [](float value) {
                       return value == 2.0F;
                   }),
            "a resource contract mismatch must preserve the prior direct graph");
    };

    expectRejectedPreservingDirect(
        exactGenerationOne,
        settings(),
        "a resource contract without its exact settings snapshot must reject");
    auto missingSummary = exactGenerationOne;
    missingSummary.resources.reset();
    expectRejectedPreservingDirect(
        std::move(missingSummary),
        resourceSettings(generationOne),
        "a resource parameter without a host-authored identity summary must reject");
    auto duplicateSettings = resourceSettings(generationOne);
    duplicateSettings.resourceSnapshots.push_back(generationOne);
    expectRejectedPreservingDirect(
        exactGenerationOne,
        std::move(duplicateSettings),
        "duplicate resource snapshots must reject");
    auto wrongId = exactGenerationOne;
    immutableResource(wrongId).resourceId = "Tables/Other";
    expectRejectedPreservingDirect(
        std::move(wrongId),
        resourceSettings(generationOne),
        "a detached resource ID must reject");
    auto wrongKind = exactGenerationOne;
    immutableResource(wrongKind).kind = talas::core::ResourceSnapshotKind::Binary;
    expectRejectedPreservingDirect(
        std::move(wrongKind),
        resourceSettings(generationOne),
        "a resource kind mismatch must reject");
    auto wrongGeneration = exactGenerationOne;
    ++immutableResource(wrongGeneration).generation;
    expectRejectedPreservingDirect(
        std::move(wrongGeneration),
        resourceSettings(generationOne),
        "a resource generation mismatch must reject");
    auto wrongSourceHash = exactGenerationOne;
    immutableResource(wrongSourceHash).sourceContentHash.assign(64, 'b');
    expectRejectedPreservingDirect(
        std::move(wrongSourceHash),
        resourceSettings(generationOne),
        "a source content hash mismatch must reject");
    auto wrongMediatedDigest = exactGenerationOne;
    immutableResource(wrongMediatedDigest).mediatedPayloadDigest.front()
        ^= 0x01U;
    expectRejectedPreservingDirect(
        std::move(wrongMediatedDigest),
        resourceSettings(generationOne),
        "a mediated payload digest mismatch must reject");
    auto wrongElementCount = exactGenerationOne;
    ++immutableResource(wrongElementCount).elementCount;
    expectRejectedPreservingDirect(
        std::move(wrongElementCount),
        resourceSettings(generationOne),
        "a table element-count mismatch must reject");
    auto wrongByteCount = exactGenerationOne;
    ++immutableResource(wrongByteCount).byteCount;
    expectRejectedPreservingDirect(
        std::move(wrongByteCount),
        resourceSettings(generationOne),
        "a mediated byte-count mismatch must reject");

    auto parameterMismatch = document;
    parameterMismatch.graph.nodes.back().parameters.front().resource =
        "Tables/Other";
    const auto parameterBinding = makeBinding(
        exactGenerationOne, 0.25F);
    require(
        bridge.compileAndStageIsolatedComponent(
                  parameterMismatch,
                  resourceSettings(generationOne),
                  parameterBinding)
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "the project resource parameter must exactly match the prepared contract");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "a project parameter mismatch must preserve the prior direct graph");

    auto generationOneDestroyed =
        std::make_shared<std::atomic<bool>>(false);
    auto generationOneDestroyedOnCallback =
        std::make_shared<std::atomic<bool>>(false);
    auto generationOneBinding = makeBinding(
        exactGenerationOne,
        0.25F,
        generationOneDestroyed,
        generationOneDestroyedOnCallback);
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationOne),
                  generationOneBinding)
                .ok(),
        "one exact immutable table contract should stage");
    generationOneBinding.reset();
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 0.0F;
               })
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               }),
        "generation one should become audible only after callback adoption");
    const auto acceptedOne = bridge.acceptedResourceIdentity(
        generationOne->id);
    require(
        acceptedOne.has_value() && acceptedOne->generation == 1
            && acceptedOne->contentHash == std::string(64, 'a'),
        "only the successfully staged resource identity should enter history");

    const auto generationTwo = immutableTableSnapshot(
        2, {0.5, 0.75, 1.0}, 'b');
    auto detachedReplacement = resourceBindingContract(
        document, *generationTwo);
    immutableResource(detachedReplacement).mediatedPayloadDigest.front()
        ^= 0x01U;
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationTwo),
                  makeBinding(std::move(detachedReplacement), 0.75F))
                .outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed,
        "a failed replacement must reject before publication");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(generationOne->id)->generation
                == 1,
        "a failed replacement must preserve old output and resource history");

    auto revokedReplacement = makeBinding(
        resourceBindingContract(document, *generationTwo), 0.75F);
    revokedReplacement->holdStageAdmission();
    std::thread revokeThread([revokedReplacement] {
        while (!revokedReplacement->stageAdmissionEntered())
            std::this_thread::yield();
        revokedReplacement->revokeAndReleaseStageAdmission();
    });
    const auto revokedStage = bridge.compileAndStageIsolatedComponent(
        document,
        resourceSettings(generationTwo),
        revokedReplacement);
    revokeThread.join();
    require(
        revokedStage.outcome == talas::runtime::ProjectStageOutcome::EngineRejected
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 2.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(generationOne->id)->generation
                == 1,
        "resource revocation before publication must preserve old output and history");

    auto generationTwoBinding = makeBinding(
        resourceBindingContract(document, *generationTwo), 0.75F);
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationTwo),
                  generationTwoBinding)
                .ok(),
        "one exact higher resource generation should stage");
    generationTwoBinding.reset();
    require(
        render(bridge, input, output, callbackActive)
            && !generationOneDestroyed->load(std::memory_order_acquire)
            && !generationOneDestroyedOnCallback->load(
                std::memory_order_acquire)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.5F) < 1.0e-6F;
               }),
        "generation two should adopt at a callback boundary without reclaiming generation one there");
    require(
        bridge.collectRetired()
            && generationOneDestroyed->load(std::memory_order_acquire)
            && !generationOneDestroyedOnCallback->load(
                std::memory_order_acquire),
        "generation-one binding and mapping owner must retire only off callback");
    const auto acceptedTwo = bridge.acceptedResourceIdentity(
        generationTwo->id);
    require(
        acceptedTwo.has_value() && acceptedTwo->generation == 2
            && acceptedTwo->contentHash == std::string(64, 'b'),
        "successful replacement should advance resource history exactly once");

    const auto generationThree = immutableBinarySnapshot(
        3, {0x00, 0x7f, 0x80, 0xfe, 0xff});
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(generationThree),
                  makeBinding(
                      resourceBindingContract(document, *generationThree),
                      1.0F))
                .ok(),
        "a higher-generation Table-to-Binary replacement should stage");
    require(
        render(bridge, input, output, callbackActive)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.0F) < 1.0e-6F;
               }),
        "Table-to-Binary replacement should adopt only at a callback boundary");
    const auto acceptedThree = bridge.acceptedResourceIdentity(
        generationThree->id);
    require(
        acceptedThree.has_value()
            && acceptedThree->kind == talas::core::ResourceSnapshotKind::Binary
            && acceptedThree->generation == 3
            && acceptedThree->contentHash == generationThree->contentHash,
        "successful Table-to-Binary replacement should advance history exactly once");
}

void testRevocationBeforePublicationPreservesPriorGraph()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedDocument();
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    auto binding = std::make_shared<ImmediateOneBlockBinding>(
        bindingContract(document),
        std::make_shared<std::atomic<bool>>(false),
        callbackActive,
        std::make_shared<std::atomic<bool>>(false));
    binding->holdStageAdmission();

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "the prior graph should stage before the revocation race");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive),
        "the prior graph should activate before the revocation race");

    std::thread revokeThread([binding] {
        while (!binding->stageAdmissionEntered())
            std::this_thread::yield();
        binding->revokeAndReleaseStageAdmission();
    });
    const auto stage = bridge.compileAndStageIsolatedComponent(
        document, settings(), binding);
    revokeThread.join();

    require(
        stage.outcome == talas::runtime::ProjectStageOutcome::EngineRejected,
        "revocation linearized before publication must reject the candidate graph");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "revocation before publication must preserve the prior audible graph");
}

void testAggregateResourceSetAdoptsAndRejectsAtomically()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedResourceSetDocument();
    const auto table = immutableTableSnapshot(
        1, {0.25, 0.5, 0.75}, 'a');
    auto binaryMutable = std::make_shared<talas::core::ResourceSnapshot>(
        *immutableBinarySnapshot(1, {0x00, 0x7f, 0xff}));
    binaryMutable->id = "Binary/Shape-B";
    const talas::core::ResourceSnapshotPtr binary = binaryMutable;
    const auto audio = immutableAudioSnapshot(1, 'c');
    const std::array<talas::core::ResourceSnapshotPtr, 3> snapshots{
        table, binary, audio};
    const auto exact = resourceSetBindingContract(document, snapshots);
    const auto makeSettings = [&]() {
        auto result = settings();
        result.resourceSnapshots.assign(snapshots.begin(), snapshots.end());
        return result;
    };
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    const auto makeBinding = [&](auto contract, float multiplier = 0.5F) {
        return std::make_shared<ImmediateOneBlockBinding>(
            std::move(contract),
            std::make_shared<std::atomic<bool>>(false),
            callbackActive,
            std::make_shared<std::atomic<bool>>(false),
            multiplier);
    };

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "a prior direct graph should stage before aggregate admission tests");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 2.0F;
               }),
        "the prior graph should be audible before aggregate staging");

    const auto expectRejected = [&](auto contract,
                                    auto candidateSettings,
                                    std::string_view message) {
        require(
            bridge.compileAndStageIsolatedComponent(
                      document,
                      std::move(candidateSettings),
                      makeBinding(std::move(contract)))
                    .outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed,
            std::string(message));
        require(
            render(bridge, input, output, callbackActive)
                && std::ranges::all_of(output, [](float value) {
                       return value == 2.0F;
                   }),
            "any aggregate mismatch must preserve the prior graph");
    };

    auto setDigestDrift = exact;
    setDigestDrift.resources->grant->resourceSetDigest.front() ^= 0x01U;
    expectRejected(
        std::move(setDigestDrift),
        makeSettings(),
        "resource-set digest drift must reject the complete graph");
    auto memberDrift = exact;
    ++memberDrift.resources->memberRecords[1].resourceGeneration;
    expectRejected(
        std::move(memberDrift),
        makeSettings(),
        "one detached member record must reject the complete graph");
    auto aggregateDrift = exact;
    ++aggregateDrift.resources->grant->aggregateWorkingBytes;
    expectRejected(
        std::move(aggregateDrift),
        makeSettings(),
        "aggregate budget drift must reject the complete graph");
    auto removedMember = exact;
    removedMember.resources->members.pop_back();
    expectRejected(
        std::move(removedMember),
        makeSettings(),
        "a removed member must reject the complete graph");
    auto reorderedMembers = exact;
    std::swap(
        reorderedMembers.resources->members[0],
        reorderedMembers.resources->members[1]);
    expectRejected(
        std::move(reorderedMembers),
        makeSettings(),
        "member order drift must reject the complete graph");
    auto reorderedSettings = makeSettings();
    std::swap(
        reorderedSettings.resourceSnapshots[0],
        reorderedSettings.resourceSnapshots[1]);
    expectRejected(
        exact,
        std::move(reorderedSettings),
        "snapshot order drift must reject the complete graph");
    auto extraSettings = makeSettings();
    extraSettings.resourceSnapshots.push_back(table);
    expectRejected(
        exact,
        std::move(extraSettings),
        "an extra snapshot must reject the complete graph");

    require(
        bridge.compileAndStageIsolatedComponent(
                  document, makeSettings(), makeBinding(exact, 0.75F))
                .ok(),
        "one exact ordered aggregate resource set should stage atomically");
    require(
        render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return value == 0.0F;
               })
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 3.5F) < 1.0e-6F;
               }),
        "the aggregate graph should adopt only at a callback boundary");
    for (const auto& snapshot : snapshots)
    {
        const auto accepted = bridge.acceptedResourceIdentity(snapshot->id);
        require(
            accepted.has_value()
                && accepted->kind == snapshot->kind
                && accepted->generation == snapshot->generation
                && accepted->contentHash == snapshot->contentHash,
            "every aggregate member must enter history together");
    }
    const auto health = bridge.streamHealth();
    require(
        health.consumerCount == 1 && health.consumers.size() == 1
            && health.consumers.front().resourceId == audio->id,
        "aggregate health should project each isolated Audio slot exactly once");

    auto tableReplacementMutable = std::make_shared<talas::core::ResourceSnapshot>(
        *immutableTableSnapshot(2, {0.1, 0.2, 0.3}, 'd'));
    tableReplacementMutable->id = binary->id;
    const talas::core::ResourceSnapshotPtr tableReplacement =
        tableReplacementMutable;
    const std::array<talas::core::ResourceSnapshotPtr, 3>
        tableDirectionSnapshots{table, tableReplacement, audio};
    const auto tableDirectionContract = resourceSetBindingContract(
        document, tableDirectionSnapshots);
    auto tableDirectionSettings = settings();
    tableDirectionSettings.resourceSnapshots.assign(
        tableDirectionSnapshots.begin(), tableDirectionSnapshots.end());
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  std::move(tableDirectionSettings),
                  makeBinding(tableDirectionContract, 1.0F))
                .ok()
            && render(bridge, input, output, callbackActive)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.0F) < 1.0e-6F;
               }),
        "a higher-generation Binary-to-Table member replacement should adopt as one set");

    auto binaryReplacementMutable = std::make_shared<talas::core::ResourceSnapshot>(
        *immutableBinarySnapshot(3, {0x10, 0x20, 0x30}));
    binaryReplacementMutable->id = binary->id;
    const talas::core::ResourceSnapshotPtr binaryReplacement =
        binaryReplacementMutable;
    const std::array<talas::core::ResourceSnapshotPtr, 3>
        binaryDirectionSnapshots{table, binaryReplacement, audio};
    const auto binaryDirectionContract = resourceSetBindingContract(
        document, binaryDirectionSnapshots);
    auto binaryDirectionSettings = settings();
    binaryDirectionSettings.resourceSnapshots.assign(
        binaryDirectionSnapshots.begin(), binaryDirectionSnapshots.end());
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  std::move(binaryDirectionSettings),
                  makeBinding(binaryDirectionContract, 1.25F))
                .ok()
            && render(bridge, input, output, callbackActive)
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.5F) < 1.0e-6F;
               })
            && bridge.acceptedResourceIdentity(binary->id)->kind
                == talas::core::ResourceSnapshotKind::Binary
            && bridge.acceptedResourceIdentity(binary->id)->generation == 3,
        "a higher-generation Table-to-Binary member replacement should adopt as one set");

    auto forbiddenAudioKindMutable =
        std::make_shared<talas::core::ResourceSnapshot>(
            *immutableTableSnapshot(2, {0.4, 0.5}, 'e'));
    forbiddenAudioKindMutable->id = audio->id;
    const talas::core::ResourceSnapshotPtr forbiddenAudioKind =
        forbiddenAudioKindMutable;
    const std::array<talas::core::ResourceSnapshotPtr, 3>
        forbiddenAudioSnapshots{table, binaryReplacement, forbiddenAudioKind};
    auto forbiddenAudioSettings = settings();
    forbiddenAudioSettings.resourceSnapshots.assign(
        forbiddenAudioSnapshots.begin(), forbiddenAudioSnapshots.end());
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  std::move(forbiddenAudioSettings),
                  makeBinding(binaryDirectionContract, 1.5F))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.5F) < 1.0e-6F;
               }),
        "an Audio cross-kind member transition must preserve the complete prior set");

    auto revokedReplacement = makeBinding(binaryDirectionContract, 1.5F);
    revokedReplacement->holdStageAdmission();
    std::thread revokeThread([revokedReplacement] {
        while (!revokedReplacement->stageAdmissionEntered())
            std::this_thread::yield();
        revokedReplacement->revokeAndReleaseStageAdmission();
    });
    auto revokedSettings = settings();
    revokedSettings.resourceSnapshots.assign(
        binaryDirectionSnapshots.begin(), binaryDirectionSnapshots.end());
    const auto revoked = bridge.compileAndStageIsolatedComponent(
        document, std::move(revokedSettings), revokedReplacement);
    revokeThread.join();
    require(
        revoked.outcome == talas::runtime::ProjectStageOutcome::EngineRejected
            && render(bridge, input, output, callbackActive)
            && std::ranges::all_of(output, [](float value) {
                   return std::abs(value - 4.5F) < 1.0e-6F;
               }),
        "revocation before aggregate publication must preserve the complete prior graph");
}

void testExplicitReplacementReceiptAdmitsOneAudioCrossKindTransition()
{
    namespace bundle = talas::runtime::component_bundle;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedResourceDocument();
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    const auto makeBinding = [&](
                                 talas::runtime::IsolatedComponentRuntimeContract
                                     contract,
                                 float multiplier) {
        return std::make_shared<ImmediateOneBlockBinding>(
            std::move(contract),
            std::make_shared<std::atomic<bool>>(false),
            callbackActive,
            std::make_shared<std::atomic<bool>>(false),
            multiplier);
    };

    talas::runtime::ProjectRuntimeBridge bridge(directDocument(), registry);
    bridge.setPlaying(true);
    const auto table = immutableTableSnapshot(1, {0.25, 0.5}, 'a');
    require(
        bridge.compileAndStageIsolatedComponent(
                  document,
                  resourceSettings(table),
                  makeBinding(resourceBindingContract(document, *table), 0.5F))
            .ok(),
        "replacement fixture should stage its initial Table generation");
    std::array<float, blockFrames> input{};
    std::array<float, blockFrames> output{};
    input.fill(2.0F);
    require(
        render(bridge, input, output, callbackActive),
        "replacement fixture should adopt its initial Table graph");

    auto audioMutable = std::make_shared<talas::core::ResourceSnapshot>(
        *immutableAudioSnapshot(2, 'b'));
    audioMutable->id = table->id;
    const talas::core::ResourceSnapshotPtr audio = audioMutable;
    const auto candidate = replacementCandidateSnapshot(document, "2.0.0");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto evidence = replacementTrustEvidence(candidate, owner);
    const std::array<bundle::ComponentBundleReplacementResourceTransition, 1>
        transitions{{{
            {
                table->id,
                table->kind,
                table->generation,
                table->contentHash,
            },
            {
                audio->id,
                audio->kind,
                audio->generation,
                audio->contentHash,
            },
            true,
        }}};
    std::array<
        std::uint8_t,
        bundle::componentBundleReplacementSessionNonceBytes> sessionNonce{};
    sessionNonce.fill(0x6d);
    bundle::ComponentBundleReplacementConfirmationStore confirmation(
        sessionNonce);
    const auto& projectNode = document.graph.nodes.back();
    const auto prepared = confirmation.prepare({
        21,
        projectNode.instanceId,
        {
            projectNode.typeId,
            &*projectNode.preservedComponent,
            &candidate,
            &evidence,
            transitions,
        },
    });
    auto receipt = confirmation.confirm(prepared.previewGeneration);
    require(
        prepared.ready() && receipt.has_value() && receipt->active(),
        "exact Audio cross-kind preview should produce one live confirmation receipt");

    const auto replacementContract = [&] {
        auto contract = audioBindingContract(document, *audio);
        contract.typeVersion = candidate.manifest.semanticVersion;
        contract.contentDigest = evidence.candidate.contentDigest;
        contract.signerIdentityDigest =
            evidence.candidate.signerIdentityDigest;
        contract.requestedPermissions =
            evidence.candidate.requestedPermissions;
        contract.policyVersion = evidence.candidate.policyVersion;
        contract.staticParameters = {resourceStaticParameter(audio->id)};
        auto& stream = std::get<
            talas::runtime::IsolatedComponentAudioStreamContract>(
                contract.resources->members.front());
        stream.parameterId = "table";
        return contract;
    };
    const auto stale = bridge.compileAndStageIsolatedComponentReplacement(
        document,
        resourceSettings(audio),
        makeBinding(replacementContract(), 1.5F),
        22,
        projectNode.instanceId,
        evidence.candidate,
        std::move(*receipt));
    require(
        stale.outcome == talas::runtime::ProjectStageOutcome::ReplacementRejected
            && receipt->active()
            && bridge.acceptedResourceIdentity(table->id)->kind
                == talas::core::ResourceSnapshotKind::Table,
        "stale document generation must preserve the prior graph, ledger, and retryable receipt");
    const auto replacement =
        bridge.compileAndStageIsolatedComponentReplacement(
            document,
            resourceSettings(audio),
            makeBinding(replacementContract(), 1.5F),
            21,
            projectNode.instanceId,
            evidence.candidate,
            std::move(*receipt));
    require(
        replacement.ok()
            && talas::project::serializeProjectDocument(
                   bridge.editor().document())
                == talas::project::serializeProjectDocument(directDocument()),
        "confirmed replacement should stage without mutating the canonical editor document");
    require(
        render(bridge, input, output, callbackActive)
            && bridge.acceptedResourceIdentity(audio->id).has_value()
            && bridge.acceptedResourceIdentity(audio->id)->kind
                == talas::core::ResourceSnapshotKind::Audio
            && bridge.acceptedResourceIdentity(audio->id)->generation == 2,
        "confirmed Audio cross-kind replacement should adopt and publish its exact ledger generation");

    const auto unconfirmedTable = immutableTableSnapshot(
        3, {0.75, 1.0}, 'c');
    const auto rejected = bridge.compileAndStageIsolatedComponent(
        document,
        resourceSettings(unconfirmedTable),
        makeBinding(
            resourceBindingContract(document, *unconfirmedTable),
            0.25F));
    require(
        rejected.outcome
                == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict
            && bridge.acceptedResourceIdentity(audio->id)->kind
                == talas::core::ResourceSnapshotKind::Audio
            && render(bridge, input, output, callbackActive),
        "unconfirmed Audio cross-kind rollback must preserve the replacement graph and ledger");
}

void testActiveBindingReclaimsOffCallbackOnBridgeShutdown()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = isolatedDocument();
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto callbackActive = std::make_shared<std::atomic<bool>>(false);
    auto destroyedOnCallback = std::make_shared<std::atomic<bool>>(false);
    auto binding = std::make_shared<ImmediateOneBlockBinding>(
        bindingContract(document),
        destroyed,
        callbackActive,
        destroyedOnCallback);
    {
        talas::runtime::ProjectRuntimeBridge bridge(document, registry);
        bridge.setPlaying(true);
        require(
            bridge.compileAndStageIsolatedComponent(
                      document, settings(), binding)
                    .ok(),
            "shutdown fixture should stage one active isolated graph");
        binding.reset();
        std::array<float, blockFrames> input{};
        std::array<float, blockFrames> output{};
        require(
            render(bridge, input, output, callbackActive),
            "shutdown fixture should adopt its active graph");
        require(
            !destroyed->load(std::memory_order_acquire),
            "active graph binding must remain live before bridge shutdown");
    }
    require(
        destroyed->load(std::memory_order_acquire)
            && !destroyedOnCallback->load(std::memory_order_acquire),
        "bridge shutdown must reclaim an active binding only off callback");
}
}

int main()
{
    testOfflineIsolatedComponentWholeCallRollback();
    testOfflineIsolatedParticipantFailureAndRetirementBoundaries();
    testOfflineIsolatedCommitFailureIsTerminal();
    testOfflineIsolatedCommitAndTerminalCleanupAllocateNothing();
    testOfflineIsolatedAllocationFailureIsAtomic();
    testTransientBindingStagesWithoutMutatingPreservedAuthority();
    testTransientBindingRetainsStereoPortOrder();
    testBinaryResourceBindingStagesExactRawIdentity();
    testAudioStreamBindingIsExactPathlessAndFailureAtomic();
    testAudioStreamHealthNeverDropsAcrossInitialGraphAdoption();
    testImmutableResourceBindingIsExactAndReplacementIsFailureAtomic();
    testAggregateResourceSetAdoptsAndRejectsAtomically();
    testExplicitReplacementReceiptAdmitsOneAudioCrossKindTransition();
    testRevocationBeforePublicationPreservesPriorGraph();
    testActiveBindingReclaimsOffCallbackOnBridgeShutdown();
    return 0;
}
