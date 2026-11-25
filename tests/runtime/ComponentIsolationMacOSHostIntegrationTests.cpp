#include "talas/runtime/ComponentIsolationMacOSHost.h"
#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <tuple>
#include <vector>

// Compiled once per integration group or fixture mode; helpers that the
// current configuration does not use are expected.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace
{
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void bindProbeState(
    ComponentTrustEvidence& evidence,
    ComponentProbeState state)
{
    if (state == ComponentProbeState::NotRun)
    {
        evidence.probe = {};
        return;
    }
    component_probe::ComponentBundleProbeRequestV1 request;
    request.header = component_probe::componentBundleProbeHeaderV1(
        component_probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.bundleIdBytes = evidence.candidate.bundleId.size();
    request.semanticVersionBytes = evidence.candidate.semanticVersion.size();
    std::copy(
        evidence.candidate.bundleId.begin(),
        evidence.candidate.bundleId.end(),
        request.bundleId.begin());
    std::copy(
        evidence.candidate.semanticVersion.begin(),
        evidence.candidate.semanticVersion.end(),
        request.semanticVersion.begin());
    request.contentDigest = evidence.candidate.contentDigest;
    request.signerIdentityDigest = evidence.candidate.signerIdentityDigest;
    request.requestedPermissions = evidence.candidate.requestedPermissions;
    request.declaredRoles = evidence.declaredRoles;
    request.policyVersion = evidence.candidate.policyVersion;
    request.entryByteCount = 4096;
    request.artifactGeneration = evidence.artifactGeneration;
    request.validationNonce = evidence.validationNonce;
    std::optional<component_probe::ComponentBundleProbeEvidenceAuthoringV1>
        authored;
    if (state == ComponentProbeState::AbnormalTermination
        || state == ComponentProbeState::TimedOut
        || state == ComponentProbeState::ProtocolError)
    {
        authored = component_probe::ComponentBundleProbeEvidenceFactory::
            makeParentClassifiedFailure(state, request);
    }
    else
    {
        component_probe::ComponentBundleProbeResultV1 result;
        result.header = component_probe::componentBundleProbeHeaderV1(
            component_probe::ComponentBundleProbeRecordKindV1::Result,
            sizeof(result));
        result.state = static_cast<std::uint32_t>(state);
        result.diagnostic = static_cast<std::uint32_t>(
            state == ComponentProbeState::Clean
                ? component_probe::ComponentBundleProbeResultDiagnosticV1::Ready
                : state == ComponentProbeState::Rejected
                ? component_probe::ComponentBundleProbeResultDiagnosticV1::
                      MetadataRejected
                : component_probe::ComponentBundleProbeResultDiagnosticV1::
                      ModuleControlledFatal);
        result.contentDigest = request.contentDigest;
        result.artifactGeneration = request.artifactGeneration;
        result.validationNonce = request.validationNonce;
        authored = component_probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(result, request);
    }
    require(authored.has_value(), "native probe fixture should author");
    evidence.probe = authored->evidence;
    static std::vector<std::unique_ptr<
        component_probe::ComponentBundleProbeValidationSessionV1>> sessions;
    sessions.push_back(std::make_unique<
        component_probe::ComponentBundleProbeValidationSessionV1>(
        std::move(authored->session)));
}

ComponentIsolationControlSessionConfigurationV1 configuration()
{
    ComponentIsolationControlSessionConfigurationV1 value;
    for (std::size_t index = 0;
         index < value.contract.validationNonce.size();
         ++index)
    {
        value.contract.validationNonce[index] =
            static_cast<std::uint8_t>(index + 9);
    }
    value.contract.sessionGeneration = 101;
    value.contract.sharedStorageIdentity = 211;
    value.contract.inputChannelCount = 2;
    value.contract.outputChannelCount = 2;
    value.contract.maximumFrames = 257;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.controlPayloadCapacityBytes = 128;
    return value;
}

ComponentTrustEvidence approvedCleanTrustEvidence()
{
    ComponentTrustEvidence evidence;
    evidence.candidate.bundleId = "org.talas.product-fixture";
    evidence.candidate.semanticVersion = "1.0.0";
    evidence.candidate.contentDigest.fill(0x5a);
    evidence.candidate.signerIdentityDigest.fill(0xa5);
    evidence.candidate.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    evidence.candidate.policyVersion = currentComponentTrustPolicyVersion;
    evidence.candidate.hasContentDigest = true;
    evidence.candidate.hasSignerIdentity = true;
    static component_bundle::ComponentOwnerAuthorizationStore authorization;
    static const auto authorized = authorization.recordOwnerAction(
        evidence.candidate,
        component_bundle::ComponentOwnerAuthorizationAction::Approve,
        authorization.generation());
    require(authorized.ok(), "product fixture approval should apply once");
    evidence.ownerAuthorizationEvidence = authorization.authorizationEvidence();
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = evidence.candidate.contentDigest;
    signature.signerIdentityDigest =
        evidence.candidate.signerIdentityDigest;
    signature.profileVersion = currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    evidence.signature = component_bundle::
        ComponentBundleSignatureEvidenceFactory::make(signature);
    evidence.artifactGeneration = 17;
    evidence.validationNonce.fill(0x3c);
    evidence.hasValidationNonce = true;
    evidence.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor);
    bindProbeState(evidence, ComponentProbeState::Clean);
    return evidence;
}

ComponentIsolationMacOSHostPreparation prepareHost(
    const ComponentIsolationControlSessionConfigurationV1& control,
    ComponentIsolationMacOSProductEndpointV1 endpoint)
{
    ComponentIsolationAudioDataPlaneConfiguration audio;
    audio.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(audio);
    require(
        preparedAudio.diagnostic
                == ComponentIsolationAudioPreparationDiagnostic::Ready
            && preparedAudio.dataPlane != nullptr,
        "native host fixture should prepare one host-owned shared audio plane");
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    return ComponentIsolationMacOSHost::prepare(
        control,
        std::move(sharedAudio),
        endpoint);
}

struct NativeAudioHarness
{
    std::shared_ptr<ComponentIsolationAudioDataPlane> audio;
    ComponentIsolationMacOSHostPreparation prepared;
};

NativeAudioHarness prepareAudioHarness(
    const ComponentIsolationControlSessionConfigurationV1& control,
    ComponentIsolationMacOSProductEndpointV1 endpoint =
        ComponentIsolationMacOSProductEndpointV1::ProductionGain)
{
    ComponentIsolationAudioDataPlaneConfiguration audioConfiguration;
    audioConfiguration.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(
        audioConfiguration);
    require(
        preparedAudio.diagnostic
                == ComponentIsolationAudioPreparationDiagnostic::Ready
            && preparedAudio.dataPlane != nullptr,
        "native audio harness should prepare its host-owned plane");
    NativeAudioHarness harness;
    harness.audio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    harness.prepared = ComponentIsolationMacOSHost::prepare(
        control,
        harness.audio,
        endpoint);
    require(
        harness.prepared.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
            && harness.prepared.host != nullptr,
        "native audio harness should prepare one sealed product endpoint");
    return harness;
}

struct NativeAudioBlock
{
    NativeAudioBlock(
        std::uint32_t channels,
        std::uint32_t frames,
        float initial)
        : storage(channels, std::vector<float>(frames, initial))
    {
        for (std::size_t channel = 0; channel < storage.size(); ++channel)
        {
            mutableChannels[channel] = storage[channel].data();
            constChannels[channel] = storage[channel].data();
        }
        channelCount = storage.size();
    }

    [[nodiscard]] std::span<const float* const> input() const noexcept
    {
        return {constChannels.data(), channelCount};
    }

    [[nodiscard]] std::span<float* const> output() noexcept
    {
        return {mutableChannels.data(), channelCount};
    }

    std::vector<std::vector<float>> storage;
    std::array<float*, maximumComponentIsolationAudioChannelsV1>
        mutableChannels{};
    std::array<const float*, maximumComponentIsolationAudioChannelsV1>
        constChannels{};
    std::size_t channelCount = 0;
};

void fillNativeInput(NativeAudioBlock& block, float seed)
{
    for (std::size_t channel = 0; channel < block.storage.size(); ++channel)
    {
        for (std::size_t frame = 0;
             frame < block.storage[channel].size();
             ++frame)
        {
            block.storage[channel][frame] = seed
                + static_cast<float>(channel * 100 + frame) * 0.001F;
        }
    }
}

bool waitForNativeResponsePublication(
    ComponentIsolationAudioDataPlane& audio,
    std::uint64_t expectedSequence)
{
    const auto layout = audio.sharedRegionLayout();
    const auto responseRegion = audio.workerResponseRegion();
    const auto slotIndex = static_cast<std::size_t>(
        (expectedSequence - 1U) % componentIsolationAudioSlotCountV1);
    auto* slot = reinterpret_cast<
        ComponentIsolationAudioSharedResponseSlotV1*>(
        responseRegion.data()
        + layout.responseSlotOffsets[slotIndex]
        - layout.responseRegionOffset);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto publication = std::atomic_ref<std::uint64_t>(
            slot->responsePublication).load(std::memory_order_acquire);
        const auto sequence = std::atomic_ref<std::uint64_t>(
            slot->response.slotSequence).load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (publication != 0 && (publication & 1U) == 0U
            && sequence == expectedSequence
            && std::atomic_ref<std::uint64_t>(slot->responsePublication).load(
                   std::memory_order_relaxed)
                == publication)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

void requireNativeGain(
    const NativeAudioBlock& input,
    const NativeAudioBlock& output)
{
    for (std::size_t channel = 0; channel < input.storage.size(); ++channel)
    {
        for (std::size_t frame = 0;
             frame < input.storage[channel].size();
             ++frame)
        {
            require(
                output.storage[channel][frame]
                    == input.storage[channel][frame] * 0.5F,
                "native XPC output should match the fixed product gain exactly");
        }
    }
}

ComponentIsolationMacOSHostPreparation prepareHost(
    ComponentIsolationMacOSProductEndpointV1 endpoint =
        ComponentIsolationMacOSProductEndpointV1::ProductionGain)
{
    return prepareHost(configuration(), endpoint);
}

std::uint32_t stateBit(ComponentIsolationControlSessionState state)
{
    return 1U << static_cast<std::uint8_t>(state);
}

bool waitForState(
    const ComponentIsolationMacOSHost& host,
    ComponentIsolationControlSessionState expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (host.state() == expected
            && (expected != ComponentIsolationControlSessionState::Ready
                || !host.operationPending()))
            return true;
        if (host.state() == ComponentIsolationControlSessionState::Quarantined)
            return expected == ComponentIsolationControlSessionState::Quarantined;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testEmbeddedProductServiceHandshakeHeartbeatAndTeardown()
{
    auto prepared = prepareHost();
    require(
        prepared.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
            && prepared.host != nullptr,
        "fixed product host configuration should prepare off callback");
    require(
        prepared.host->start()
            == ComponentIsolationMacOSHostOperation::Scheduled,
        "sealed launcher should schedule its fixed embedded service");
    const auto becameReady = waitForState(
        *prepared.host,
        ComponentIsolationControlSessionState::Ready);
    if (!becameReady)
    {
        std::cerr << "handshake state="
                  << static_cast<int>(prepared.host->state())
                  << " diagnostic="
                  << static_cast<int>(prepared.host->diagnostic()) << '\n';
    }
    require(
        becameReady,
        "embedded product service should complete the exact handshake");
    const auto liveIsolation = prepared.host->isolationEvidence();
    const auto liveView = liveIsolation.inspect();
    require(
        liveView.state == ComponentIsolationState::Ready
            && liveView.profileVersion
                == currentComponentIsolationProfileVersion
            && liveView.safeguards == allComponentIsolationSafeguardsV1
            && liveView.supportedPermissions
                == componentPermission(
                    ComponentExecutionPermission::RealtimeAudio)
            && liveView.sessionGeneration
                == configuration().contract.sessionGeneration
            && liveView.sharedStorageIdentity
                == configuration().contract.sharedStorageIdentity
            && liveView.validationNonce
                == configuration().contract.validationNonce
            && liveView.hasValidationNonce,
        "only the ready production session should derive complete bound isolation evidence");
    auto trust = approvedCleanTrustEvidence();
    trust.isolationEvidence = liveIsolation;
    require(
        evaluateComponentTrust(trust)
            == ComponentTrustDecision{
                ComponentTrustDisposition::EligibleForIsolatedActivation,
                ComponentTrustDiagnosticCode::Ready},
        "live production-backend evidence should satisfy only the containment leg of the closed evaluator");
    std::size_t activationCount = 0;
    for (const auto signature : {
             ComponentSignatureState::Missing,
             ComponentSignatureState::Verified,
             ComponentSignatureState::Invalid,
             ComponentSignatureState::Unsupported,
             ComponentSignatureState::Expired,
             ComponentSignatureState::Revoked,
         })
    {
        for (const auto probe : {
                 ComponentProbeState::NotRun,
                 ComponentProbeState::Clean,
                 ComponentProbeState::Rejected,
                 ComponentProbeState::ControlledFatal,
                 ComponentProbeState::AbnormalTermination,
                 ComponentProbeState::TimedOut,
                 ComponentProbeState::ProtocolError,
             })
        {
            auto combination = approvedCleanTrustEvidence();
            auto signatureView = combination.signature.inspect();
            signatureView.state = signature;
            combination.signature = component_bundle::
                ComponentBundleSignatureEvidenceFactory::make(signatureView);
            bindProbeState(combination, probe);
            combination.isolationEvidence = liveIsolation;
            const auto decision = evaluateComponentTrust(combination);
            if (decision.disposition
                == ComponentTrustDisposition::EligibleForIsolatedActivation)
            {
                ++activationCount;
                require(
                    signature == ComponentSignatureState::Verified
                        && probe == ComponentProbeState::Clean,
                    "live containment must not weaken another trust leg");
            }
        }
    }
    require(
        activationCount == 1,
        "the complete signature/probe matrix should have one live-contained eligible state");
    require(
        prepared.host->requestHeartbeat()
            == ComponentIsolationMacOSHostOperation::Scheduled,
        "ready embedded service should accept one bounded heartbeat");
    require(
        waitForState(
            *prepared.host,
            ComponentIsolationControlSessionState::Ready),
        "exact heartbeat reply should preserve readiness");
    require(
        prepared.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && liveIsolation.inspect()
                == ComponentIsolationEvidenceViewV1{},
        "accepted teardown should revoke evidence before dispatch");
    require(
        waitForState(
            *prepared.host,
            ComponentIsolationControlSessionState::Stopped),
        "exact teardown reply should stop without quarantine");
    require(
        prepared.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::Ready
            && prepared.host->diagnosticBytes() == 0,
        "clean service lifecycle should expose no untrusted diagnostic text");
    require(
        liveIsolation.inspect().state == ComponentIsolationState::Unavailable
            && evaluateComponentTrust(trust)
                == ComponentTrustDecision{
                    ComponentTrustDisposition::PreserveOnly,
                    ComponentTrustDiagnosticCode::IsolationUnavailable},
        "copied evidence should revoke itself when its exact live session stops");
    const auto trace = prepared.host->lifecycleTrace();
    require(
        (trace & stateBit(ComponentIsolationControlSessionState::Launching)) != 0
            && (trace
                    & stateBit(
                        ComponentIsolationControlSessionState::Handshaking))
                != 0
            && (trace
                    & stateBit(
                        ComponentIsolationControlSessionState::Prewarming))
                != 0
            && (trace & stateBit(ComponentIsolationControlSessionState::Ready))
                != 0
            && (trace
                    & stateBit(ComponentIsolationControlSessionState::Draining))
                != 0
            && (trace
                    & stateBit(ComponentIsolationControlSessionState::Stopped))
                != 0,
        "one control-queue owner should record the complete clean lifecycle");
}

void testNativeProductAudioAcrossPartitionsChannelsAndGenerations()
{
    static_assert(
        componentIsolationTransportPipelineLatencyQuantaV1 == 1);
    for (const auto channelCount : {1U, 2U, 8U})
    {
        auto control = configuration();
        control.contract.inputChannelCount = channelCount;
        control.contract.outputChannelCount = channelCount;
        control.contract.maximumFrames = 4'096;
        control.contract.sessionGeneration += channelCount;
        control.contract.sharedStorageIdentity += channelCount;
        control.contract.validationNonce.front() +=
            static_cast<std::uint8_t>(channelCount);
        auto harness = prepareAudioHarness(control);
        require(
            harness.prepared.host->start()
                    == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *harness.prepared.host,
                    ComponentIsolationControlSessionState::Ready),
            "native product audio harness should complete handshake and prewarm");

        NativeAudioBlock initialOutput(channelCount, 1, -77.0F);
        require(
            harness.audio->consumeOutput(initialOutput.output(), 1)
                    == ComponentIsolationAudioOperation::Missing
                && initialOutput.storage.front().front() == -77.0F,
            "the first transport quantum should be missing and preserve output");

        std::uint64_t expectedSequence = 1;
        for (const auto frameCount : {1U, 7U, 64U, 257U, 4'096U})
        {
            NativeAudioBlock input(channelCount, frameCount, 0.0F);
            NativeAudioBlock output(channelCount, frameCount, -88.0F);
            fillNativeInput(
                input,
                static_cast<float>(channelCount * 10 + frameCount) * 0.01F);
            require(
                harness.audio->publishInput(input.input(), frameCount)
                        == ComponentIsolationAudioOperation::Published
                    && waitForNativeResponsePublication(
                        *harness.audio,
                        expectedSequence)
                    && harness.audio->consumeOutput(output.output(), frameCount)
                        == ComponentIsolationAudioOperation::Ready,
                "the asynchronous native transport should return one exact sequence response");
            requireNativeGain(input, output);
            ++expectedSequence;
        }
        require(
            harness.prepared.host->requestTeardown()
                    == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *harness.prepared.host,
                    ComponentIsolationControlSessionState::Stopped)
                && harness.audio->isQuarantined(),
            "clean stop should retire the exact shared-audio generation");
    }

    auto oldControl = configuration();
    oldControl.contract.sessionGeneration = 801;
    oldControl.contract.sharedStorageIdentity = 901;
    oldControl.contract.validationNonce.fill(0x31);
    oldControl.contract.maximumFrames = 64;
    auto oldGeneration = prepareAudioHarness(oldControl);
    require(
        oldGeneration.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *oldGeneration.prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "old generation should become independently ready");

    auto replacementControl = oldControl;
    ++replacementControl.contract.sessionGeneration;
    ++replacementControl.contract.sharedStorageIdentity;
    ++replacementControl.contract.validationNonce.front();
    auto replacement = prepareAudioHarness(replacementControl);
    require(
        replacement.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *replacement.prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "replacement generation should stage without disturbing the old session");
    NativeAudioBlock replacementInput(2, 64, 0.0F);
    NativeAudioBlock replacementOutput(2, 64, -99.0F);
    fillNativeInput(replacementInput, 3.0F);
    require(
        replacement.audio->publishInput(replacementInput.input(), 64)
                == ComponentIsolationAudioOperation::Published
            && waitForNativeResponsePublication(*replacement.audio, 1)
            && replacement.audio->consumeOutput(
                replacementOutput.output(),
                64)
                == ComponentIsolationAudioOperation::Ready,
        "replacement generation should render through its own helper and storage");
    requireNativeGain(replacementInput, replacementOutput);
    require(
        oldGeneration.prepared.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *oldGeneration.prepared.host,
                ComponentIsolationControlSessionState::Stopped)
            && oldGeneration.audio->isQuarantined()
            && replacement.prepared.host->state()
                == ComponentIsolationControlSessionState::Ready
            && replacement.prepared.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *replacement.prepared.host,
                ComponentIsolationControlSessionState::Stopped),
        "generation swap should retire only the old storage before clean replacement teardown");
}

void testHostBoundAudioRemainsInactiveUntilReady()
{
    const auto control = configuration();
    auto harness = prepareAudioHarness(control);
    NativeAudioBlock input(2, 64, 0.25F);
    NativeAudioBlock output(2, 64, -1.0F);
    require(
        harness.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Inactive
            && harness.audio->consumeOutput(output.output(), 64)
                == ComponentIsolationAudioOperation::Inactive
            && harness.audio->replaceGeneration(control.contract)
                == ComponentIsolationAudioOperation::InvalidState,
        "a host-bound plane must reject audio and public generation replacement before readiness");
    require(
        harness.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *harness.prepared.host,
                ComponentIsolationControlSessionState::Ready)
            && harness.audio->replaceGeneration(control.contract)
                == ComponentIsolationAudioOperation::InvalidState
            && harness.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Published
            && waitForNativeResponsePublication(*harness.audio, 1)
            && harness.audio->consumeOutput(output.output(), 64)
                == ComponentIsolationAudioOperation::Ready,
        "the exact plane should activate only after handshake and prewarm");
    requireNativeGain(input, output);
    require(
        harness.prepared.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *harness.prepared.host,
                ComponentIsolationControlSessionState::Stopped),
        "host-bound activation fixture should stop cleanly");
}

void testConcurrentHostBindingQuiescesRetainedPlaneOperations()
{
    for (std::uint64_t trial = 0; trial < 64; ++trial)
    {
        auto control = configuration();
        control.contract.sessionGeneration += trial * 2;
        control.contract.sharedStorageIdentity += trial * 2;
        ComponentIsolationAudioDataPlaneConfiguration audioConfiguration;
        audioConfiguration.contract = control.contract;
        auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(
            audioConfiguration);
        require(
            preparedAudio.diagnostic
                    == ComponentIsolationAudioPreparationDiagnostic::Ready
                && preparedAudio.dataPlane != nullptr,
            "concurrent binding fixture should prepare standalone storage");
        auto audio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
            std::move(preparedAudio.dataPlane));
        auto replacement = control.contract;
        ++replacement.sessionGeneration;
        ++replacement.sharedStorageIdentity;
        ++replacement.validationNonce.front();

        std::atomic<bool> begin{false};
        std::atomic<bool> stopPublisher{false};
        std::atomic<ComponentIsolationAudioOperation> replacementResult{
            ComponentIsolationAudioOperation::InvalidArgument};
        NativeAudioBlock input(2, 64, 0.25F);
        NativeAudioBlock output(2, 64, -1.0F);
        std::thread publisher([&] {
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!stopPublisher.load(std::memory_order_acquire))
            {
                if (audio->publishInput(input.input(), 64)
                    == ComponentIsolationAudioOperation::Published)
                {
                    static_cast<void>(
                        audio->consumeOutput(output.output(), 64));
                }
            }
        });
        std::thread replacer([&] {
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            replacementResult.store(
                audio->replaceGeneration(replacement),
                std::memory_order_release);
        });

        begin.store(true, std::memory_order_release);
        auto prepared = ComponentIsolationMacOSHost::prepare(
            control,
            audio,
            ComponentIsolationMacOSProductEndpointV1::ProductionGain);
        replacer.join();
        stopPublisher.store(true, std::memory_order_release);
        publisher.join();

        const auto replacementOperation = replacementResult.load(
            std::memory_order_acquire);
        const auto hostBound = prepared.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
            && prepared.host != nullptr;
        require(
            !(hostBound
                && replacementOperation
                    == ComponentIsolationAudioOperation::Replaced),
            "host binding and generation replacement must have one atomic winner");
        if (hostBound)
        {
            require(
                replacementOperation
                        == ComponentIsolationAudioOperation::InvalidState
                    && audio->publishInput(input.input(), 64)
                        == ComponentIsolationAudioOperation::Inactive,
                "successful host binding must quiesce and close every retained operation");
        }
        else
        {
            require(
                prepared.diagnostic
                        == ComponentIsolationMacOSHostPreparationDiagnostic::
                            InvalidConfiguration
                    && replacementOperation
                        == ComponentIsolationAudioOperation::Replaced,
                "a winning replacement must make the stale host configuration fail closed");
        }
    }
}

void testCallbackDetectedAudioQuarantineRevokesLiveEvidence()
{
    auto harness = prepareAudioHarness(configuration());
    require(
        harness.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *harness.prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "callback quarantine fixture should first become ready");
    const auto liveEvidence = harness.prepared.host->isolationEvidence();
    NativeAudioBlock input(2, 64, 0.25F);
    NativeAudioBlock output(2, 64, -1.0F);
    require(
        liveEvidence.inspect().state == ComponentIsolationState::Ready
            && harness.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Published
            && waitForNativeResponsePublication(*harness.audio, 1),
        "callback quarantine fixture should publish one exact response");
    const auto layout = harness.audio->sharedRegionLayout();
    auto responseRegion = harness.audio->workerResponseRegion();
    auto* hostileOutput = reinterpret_cast<float*>(
        responseRegion.data() + layout.outputSampleOffsets.front()
        - layout.responseRegionOffset);
    std::atomic_ref<float>(*hostileOutput).store(
        std::numeric_limits<float>::quiet_NaN(),
        std::memory_order_relaxed);
    require(
        harness.audio->consumeOutput(output.output(), 64)
                == ComponentIsolationAudioOperation::Quarantined
            && harness.audio->quarantineReason()
                == ComponentIsolationAudioQuarantineReason::
                    ResponseAudioNonFinite
            && liveEvidence.inspect() == ComponentIsolationEvidenceViewV1{},
        "callback-detected response quarantine must revoke the same live evidence lease");
}

void testMalformedAndProcessFailureFixturesQuarantineOrRemainBounded()
{
    for (const auto endpoint : {
             ComponentIsolationMacOSProductEndpointV1::FixtureMalformed,
             ComponentIsolationMacOSProductEndpointV1::FixtureWrongVersion,
             ComponentIsolationMacOSProductEndpointV1::FixtureWrongNonce,
             ComponentIsolationMacOSProductEndpointV1::FixtureStaleGeneration,
             ComponentIsolationMacOSProductEndpointV1::FixtureCrash,
             ComponentIsolationMacOSProductEndpointV1::FixtureEarlyExit,
         })
    {
        auto harness = prepareAudioHarness(configuration(), endpoint);
        require(
            harness.prepared.diagnostic
                    == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
                && harness.prepared.host->start()
                    == ComponentIsolationMacOSHostOperation::Scheduled,
            "every compiled failure fixture should use the sealed launcher");
        NativeAudioBlock preReadyInput(2, 64, 0.25F);
        NativeAudioBlock preReadyOutput(2, 64, -1.0F);
        const auto publication = harness.audio->publishInput(
            preReadyInput.input(),
            64);
        const auto consumption = harness.audio->consumeOutput(
            preReadyOutput.output(),
            64);
        require(
            (publication == ComponentIsolationAudioOperation::Inactive
                || publication
                    == ComponentIsolationAudioOperation::Quarantined)
                && (consumption == ComponentIsolationAudioOperation::Inactive
                    || consumption
                        == ComponentIsolationAudioOperation::Quarantined),
            "a malformed or failing handshake must never expose pre-ready audio");
        require(
            waitForState(
                *harness.prepared.host,
                ComponentIsolationControlSessionState::Quarantined)
                && harness.audio->isQuarantined(),
            "malformed, crashed, and early-exit helpers should quarantine the host and exact audio generation");
        require(
            harness.prepared.host->diagnostic()
                != ComponentIsolationControlSessionDiagnostic::Ready,
            "a failed process must retain a non-ready typed reason");
    }

    auto hanging = prepareAudioHarness(
        configuration(),
        ComponentIsolationMacOSProductEndpointV1::FixtureHang);
    require(
        hanging.prepared.host->start()
            == ComponentIsolationMacOSHostOperation::Scheduled,
        "the compiled hang fixture should launch without caller text");
    NativeAudioBlock hangingInput(2, 64, 0.25F);
    NativeAudioBlock hangingOutput(2, 64, -1.0F);
    require(
        hanging.audio->publishInput(hangingInput.input(), 64)
                == ComponentIsolationAudioOperation::Inactive
            && hanging.audio->consumeOutput(hangingOutput.output(), 64)
                == ComponentIsolationAudioOperation::Inactive,
        "a withheld handshake must leave its exact audio plane inactive");
    require(
        waitForState(
            *hanging.prepared.host,
            ComponentIsolationControlSessionState::Quarantined)
            && hanging.prepared.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::HandshakeTimeout
            && hanging.audio->isQuarantined(),
        "withheld handshake reply should quarantine its exact audio generation at the fixed watchdog");
}

void testTeardownFailureFixturesStayBoundedAndFailClosed()
{
    auto controlled = prepareHost(
        ComponentIsolationMacOSProductEndpointV1::FixtureControlledFailure);
    const auto controlledStarted = controlled.host->start()
        == ComponentIsolationMacOSHostOperation::Scheduled;
    const auto controlledReady = controlledStarted
        && waitForState(
            *controlled.host,
            ComponentIsolationControlSessionState::Ready);
    if (!controlledReady)
    {
        std::cerr << "controlled state="
                  << static_cast<int>(controlled.host->state())
                  << " diagnostic="
                  << static_cast<int>(controlled.host->diagnostic()) << '\n';
    }
    require(
        controlledReady,
        "controlled-failure helper should first complete the exact handshake");
    require(
        controlled.host->isolationEvidence().inspect().state
            == ComponentIsolationState::Unavailable,
        "compiled failure endpoints must never derive production isolation evidence");
    require(
        controlled.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *controlled.host,
                ComponentIsolationControlSessionState::Quarantined),
        "controlled teardown failure should quarantine without killing the host");
    require(
        controlled.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::RemoteFailure
            && controlled.host->diagnosticBytes() != 0
            && controlled.host->diagnosticBytes() <= 128,
        "remote diagnostic bytes should remain product-owned and host-bounded");

    for (const auto endpoint : {
             ComponentIsolationMacOSProductEndpointV1::
                 FixtureOversizedDiagnostic,
             ComponentIsolationMacOSProductEndpointV1::FixtureTeardownRace,
         })
    {
        auto prepared = prepareHost(endpoint);
        require(
            prepared.host->start()
                    == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *prepared.host,
                    ComponentIsolationControlSessionState::Ready),
            "teardown protocol fixture should first complete handshake");
        require(
            prepared.host->requestTeardown()
                    == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *prepared.host,
                    ComponentIsolationControlSessionState::Quarantined),
            "oversized or stale teardown evidence should fail closed");
    }
}

void testUnknownEndpointAndRepeatedCommandsNeverLaunch()
{
    const auto unknown = prepareHost(
        static_cast<ComponentIsolationMacOSProductEndpointV1>(0xff));
    require(
        unknown.host == nullptr
            && unknown.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::
                    InvalidConfiguration,
        "unknown endpoint enums should reject before queue or process launch");

    auto prepared = prepareHost();
    require(
        prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && prepared.host->start()
                == ComponentIsolationMacOSHostOperation::InvalidState,
        "one host instance should schedule exactly one launch");
    require(
        waitForState(
            *prepared.host,
            ComponentIsolationControlSessionState::Ready),
        "single-launch fixture should still complete clean handshake");
    require(
        prepared.host->requestHeartbeat()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && prepared.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::InvalidState,
        "overlapping control requests should reject before XPC publication");
    require(
        waitForState(
            *prepared.host,
            ComponentIsolationControlSessionState::Ready),
        "rejected overlapping command should not disturb exact heartbeat");
    require(
        prepared.host->requestTeardown()
            == ComponentIsolationMacOSHostOperation::Scheduled,
        "clean session should remain tear-downable after heartbeat");
    require(
        waitForState(
            *prepared.host,
            ComponentIsolationControlSessionState::Stopped),
        "clean single-launch session should stop");
}

void testUnsupportedPermissionAndQuarantineNeverRetainReadyEvidence()
{
    auto unsupportedConfiguration = configuration();
    unsupportedConfiguration.requestedPermissions = componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    auto unsupported = prepareHost(
        unsupportedConfiguration,
        ComponentIsolationMacOSProductEndpointV1::ProductionGain);
    const ComponentIsolationEvidenceV1 unavailable;
    require(
        unsupported.host == nullptr
            && unsupported.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::
                    InvalidConfiguration
            && unavailable.inspect().state
                == ComponentIsolationState::Unavailable,
        "unsupported authority must reject before a live evidence source exists");

    auto prepared = prepareHost();
    require(
        prepared.host->start() == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "quarantine evidence fixture should first become ready");
    const auto liveIsolation = prepared.host->isolationEvidence();
    const auto expectedContract = configuration().contract;
    std::atomic<bool> stopReader{false};
    std::atomic<bool> observedInvalidView{false};
    std::thread reader([&] {
        while (!stopReader.load(std::memory_order_acquire))
        {
            const auto view = liveIsolation.inspect();
            const auto completeReady =
                view.state == ComponentIsolationState::Ready
                && view.profileVersion
                    == currentComponentIsolationProfileVersion
                && view.safeguards == allComponentIsolationSafeguardsV1
                && view.supportedPermissions
                    == componentPermission(
                        ComponentExecutionPermission::RealtimeAudio)
                && view.sessionGeneration
                    == expectedContract.sessionGeneration
                && view.sharedStorageIdentity
                    == expectedContract.sharedStorageIdentity
                && view.validationNonce == expectedContract.validationNonce
                && view.hasValidationNonce;
            const auto completeUnavailable =
                view == ComponentIsolationEvidenceViewV1{};
            if (!completeReady && !completeUnavailable)
            {
                observedInvalidView.store(true, std::memory_order_release);
                return;
            }
        }
    });
    require(
        liveIsolation.inspect().state == ComponentIsolationState::Ready
            && prepared.host->reportResponseDeadlineMiss()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && liveIsolation.inspect()
                == ComponentIsolationEvidenceViewV1{}
            && waitForState(
                *prepared.host,
                ComponentIsolationControlSessionState::Quarantined),
        "exact-generation deadline miss should quarantine live evidence");
    stopReader.store(true, std::memory_order_release);
    reader.join();
    require(
        !observedInvalidView.load(std::memory_order_acquire)
            && liveIsolation.inspect()
                == ComponentIsolationEvidenceViewV1{},
        "copied live evidence must revoke itself on exact-generation quarantine");

    auto direct = prepareAudioHarness(configuration());
    require(
        direct.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *direct.prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "direct plane-revocation fixture should first become ready");
    const auto directEvidence = direct.prepared.host->isolationEvidence();
    require(
        directEvidence.inspect().state == ComponentIsolationState::Ready,
        "direct plane-revocation fixture should publish live evidence");
    direct.audio->quarantine();
    require(
        directEvidence.inspect() == ComponentIsolationEvidenceViewV1{},
        "audio-plane quarantine must synchronously revoke copied isolation evidence");
}

void testMismatchedSharedStorageRejectsBeforeLaunch()
{
    const auto control = configuration();
    ComponentIsolationAudioDataPlaneConfiguration audio;
    audio.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(audio);
    require(
        preparedAudio.dataPlane != nullptr,
        "mismatch fixture should prepare its original shared contract");
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    auto mismatched = control;
    ++mismatched.contract.sharedStorageIdentity;
    const auto prepared = ComponentIsolationMacOSHost::prepare(
        mismatched,
        std::move(sharedAudio));
    require(
        prepared.host == nullptr
            && prepared.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::
                    InvalidConfiguration,
        "mismatched host/shared-storage evidence should reject before queue or helper launch");
}

void testPhaseWatchdogsAndSharedStorageLifetime()
{
    auto prewarm = prepareHost(
        ComponentIsolationMacOSProductEndpointV1::FixturePrewarmHang);
    require(
        prewarm.host->start() == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *prewarm.host,
                ComponentIsolationControlSessionState::Quarantined)
            && prewarm.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::PrewarmTimeout
            && (prewarm.host->lifecycleTrace()
                    & stateBit(
                        ComponentIsolationControlSessionState::Prewarming))
                != 0
            && (prewarm.host->lifecycleTrace()
                    & stateBit(ComponentIsolationControlSessionState::Ready))
                == 0,
        "withheld prewarm reply should quarantine before readiness publication");

    for (const auto [endpoint, operation, expectedDiagnostic] : {
             std::tuple{
                 ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatHang,
                 ComponentIsolationRecordKind::Heartbeat,
                 ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout},
             std::tuple{
                 ComponentIsolationMacOSProductEndpointV1::FixtureTeardownHang,
                 ComponentIsolationRecordKind::TeardownRequest,
                 ComponentIsolationControlSessionDiagnostic::TeardownTimeout},
         })
    {
        auto prepared = prepareHost(endpoint);
        require(
            prepared.host->start()
                    == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *prepared.host,
                    ComponentIsolationControlSessionState::Ready),
            "phase watchdog fixture should become ready after prewarm");
        const auto scheduled = operation == ComponentIsolationRecordKind::Heartbeat
            ? prepared.host->requestHeartbeat()
            : prepared.host->requestTeardown();
        require(
            scheduled == ComponentIsolationMacOSHostOperation::Scheduled
                && waitForState(
                    *prepared.host,
                    ComponentIsolationControlSessionState::Quarantined)
                && prepared.host->diagnostic() == expectedDiagnostic,
            "phase watchdog should retain its exact timeout diagnostic");
        require(
            prepared.host->start()
                    == ComponentIsolationMacOSHostOperation::InvalidState
                && prepared.host->requestHeartbeat()
                    == ComponentIsolationMacOSHostOperation::InvalidState
                && prepared.host->requestTeardown()
                    == ComponentIsolationMacOSHostOperation::InvalidState,
            "timed-out generation should require explicit reprepare before restart");
    }

    const auto control = configuration();
    ComponentIsolationAudioDataPlaneConfiguration audioConfiguration;
    audioConfiguration.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(
        audioConfiguration);
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    std::weak_ptr<ComponentIsolationAudioDataPlane> lifetime = sharedAudio;
    auto prepared = ComponentIsolationMacOSHost::prepare(
        control,
        sharedAudio,
        ComponentIsolationMacOSProductEndpointV1::FixtureHang);
    sharedAudio.reset();
    require(
        !lifetime.expired()
            && prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *prepared.host,
                ComponentIsolationControlSessionState::Quarantined)
            && !lifetime.expired()
            && lifetime.lock()->isQuarantined(),
        "host should retain and quarantine shared storage through timeout quiescence");
    prepared.host.reset();
    require(
        lifetime.expired(),
        "shared storage should reclaim only after off-callback host quiescence");
}

void testResponseDeadlineAndLatePublicationRemainGenerationScoped()
{
    auto responseDeadline = prepareHost(
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatLate);
    require(
        responseDeadline.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *responseDeadline.host,
                ComponentIsolationControlSessionState::Ready),
        "response-deadline fixture should complete prewarm");
    require(
        responseDeadline.host->reportResponseDeadlineMiss()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && responseDeadline.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::InvalidState
            && waitForState(
                *responseDeadline.host,
                ComponentIsolationControlSessionState::Quarantined)
            && responseDeadline.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::ResponseDeadlineMiss,
        "one off-callback response miss should quarantine the exact ready generation");

    auto late = prepareHost(
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatLate);
    require(
        late.host->start() == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *late.host,
                ComponentIsolationControlSessionState::Ready)
            && late.host->requestHeartbeat()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *late.host,
                ComponentIsolationControlSessionState::Quarantined)
            && late.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout,
        "late helper reply should first lose the fixed heartbeat deadline");
    auto replacementConfiguration = configuration();
    ++replacementConfiguration.contract.sessionGeneration;
    ++replacementConfiguration.contract.sharedStorageIdentity;
    ++replacementConfiguration.contract.validationNonce.front();
    auto replacement = prepareHost(
        replacementConfiguration,
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatLate);
    require(
        replacement.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *replacement.host,
                ComponentIsolationControlSessionState::Ready),
        "explicit reprepare should publish only a fresh independent generation");
    std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
    require(
        late.host->state()
                == ComponentIsolationControlSessionState::Quarantined
            && late.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout
            && replacement.host->state()
                == ComponentIsolationControlSessionState::Ready,
        "late helper publication must not revive the old or disturb the replacement generation");
    require(
        replacement.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && replacement.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::InvalidState
            && waitForState(
                *replacement.host,
                ComponentIsolationControlSessionState::Stopped),
        "fresh generation should retain one bounded clean teardown");
}

void testActiveAudioQuarantinesOnTimeoutAndOwnerShutdown()
{
    auto timeoutControl = configuration();
    timeoutControl.contract.maximumFrames = 64;
    timeoutControl.contract.sessionGeneration = 1'201;
    timeoutControl.contract.sharedStorageIdentity = 1'301;
    timeoutControl.contract.validationNonce.fill(0x51);
    auto timeout = prepareAudioHarness(
        timeoutControl,
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatHang);
    require(
        timeout.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *timeout.prepared.host,
                ComponentIsolationControlSessionState::Ready),
        "active-audio timeout fixture should become ready");
    NativeAudioBlock input(2, 64, 0.0F);
    NativeAudioBlock output(2, 64, -101.0F);
    fillNativeInput(input, 4.0F);
    require(
        timeout.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Published
            && waitForNativeResponsePublication(*timeout.audio, 1)
            && timeout.audio->consumeOutput(output.output(), 64)
                == ComponentIsolationAudioOperation::Ready,
        "heartbeat-timeout fixture should render before its control failure");
    requireNativeGain(input, output);
    require(
        timeout.prepared.host->requestHeartbeat()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *timeout.prepared.host,
                ComponentIsolationControlSessionState::Quarantined)
            && timeout.prepared.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout
            && timeout.audio->isQuarantined()
            && timeout.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Quarantined,
        "heartbeat timeout must retire the same active audio generation");

    auto shutdownControl = timeoutControl;
    ++shutdownControl.contract.sessionGeneration;
    ++shutdownControl.contract.sharedStorageIdentity;
    ++shutdownControl.contract.validationNonce.front();
    auto shutdown = prepareAudioHarness(
        shutdownControl,
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatHang);
    require(
        shutdown.prepared.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *shutdown.prepared.host,
                ComponentIsolationControlSessionState::Ready)
            && shutdown.audio->publishInput(input.input(), 64)
                == ComponentIsolationAudioOperation::Published
            && waitForNativeResponsePublication(*shutdown.audio, 1),
        "owner-shutdown fixture should have one live product audio request");
    shutdown.prepared.host.reset();
    require(
        shutdown.audio->isQuarantined()
            && shutdown.audio->consumeOutput(output.output(), 64)
                == ComponentIsolationAudioOperation::Quarantined,
        "synchronous host destruction must quiesce and retire active audio");
}

void testDestructionQuiescesPendingWatchdogAndReply()
{
    const auto control = configuration();
    ComponentIsolationAudioDataPlaneConfiguration audioConfiguration;
    audioConfiguration.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(
        audioConfiguration);
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    std::weak_ptr<ComponentIsolationAudioDataPlane> lifetime = sharedAudio;
    auto prepared = ComponentIsolationMacOSHost::prepare(
        control,
        sharedAudio,
        ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatLate);
    sharedAudio.reset();
    require(
        prepared.host->start() == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *prepared.host,
                ComponentIsolationControlSessionState::Ready)
            && prepared.host->requestHeartbeat()
                == ComponentIsolationMacOSHostOperation::Scheduled,
        "destruction fixture should schedule a live watchdog/reply pair");
    prepared.host.reset();
    require(
        lifetime.expired(),
        "host destruction should synchronously quiesce and reclaim shared storage");
    std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
}
}

int main()
{
#if TALAS_COMPONENT_ISOLATION_INTEGRATION_GROUP == 1
    testEmbeddedProductServiceHandshakeHeartbeatAndTeardown();
    testNativeProductAudioAcrossPartitionsChannelsAndGenerations();
    testHostBoundAudioRemainsInactiveUntilReady();
    testConcurrentHostBindingQuiescesRetainedPlaneOperations();
    testCallbackDetectedAudioQuarantineRevokesLiveEvidence();
    testUnknownEndpointAndRepeatedCommandsNeverLaunch();
    testUnsupportedPermissionAndQuarantineNeverRetainReadyEvidence();
    testMismatchedSharedStorageRejectsBeforeLaunch();
    testMalformedAndProcessFailureFixturesQuarantineOrRemainBounded();
#else
#if TALAS_COMPONENT_ISOLATION_INTEGRATION_GROUP == 2
    testTeardownFailureFixturesStayBoundedAndFailClosed();
#else
    testPhaseWatchdogsAndSharedStorageLifetime();
    testResponseDeadlineAndLatePublicationRemainGenerationScoped();
    testActiveAudioQuarantinesOnTimeoutAndOwnerShutdown();
    testDestructionQuiescesPendingWatchdogAndReply();
#endif
#endif
    return 0;
}
