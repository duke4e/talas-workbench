#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::array<std::uint8_t, componentTrustDigestBytes> digest(
    const std::vector<std::uint8_t>& bytes)
{
    std::array<std::uint8_t, componentTrustDigestBytes> value{};
    require(
        !bytes.empty()
            && CC_SHA256(
                   bytes.data(),
                   static_cast<CC_LONG>(bytes.size()),
                   value.data()) != nullptr,
        "test payload digest should compute");
    return value;
}

bundle::ComponentBundleSnapshot snapshot()
{
    bundle::ComponentBundleSnapshot value;
    value.manifest.bundleId = "org.talas.coordinator-fixture";
    value.manifest.semanticVersion = "1.0.0";
    value.manifest.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.manifest.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.manifest.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.manifest.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    value.manifest.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.manifest.entry.path = "Payload/entry.bundle";
    const std::vector<std::uint8_t> entry{0xca, 0xfe, 0xba, 0xbe};
    value.manifest.entry.byteCount = entry.size();
    value.manifest.entry.sha256 = digest(entry);
    bundle::ComponentBundleNode node;
    node.typeId = "org.talas.coordinator-fixture.oscillator";
    value.manifest.nodes.push_back(std::move(node));
    value.manifest.state.currentSchemaVersion = 1;
    value.manifest.provenance.publisher = "Talas tests";
    value.manifest.provenance.licenseExpression = "Apache-2.0";
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        value.manifest);
    require(canonical.ok(), "coordinator manifest should serialize");
    value.canonicalManifest = canonical.canonicalJson;
    bundle::ComponentBundleFrozenPayload payload;
    payload.path = value.manifest.entry.path;
    payload.sha256 = value.manifest.entry.sha256;
    payload.bytes = entry;
    payload.isEntry = true;
    value.payloads.push_back(std::move(payload));
    value.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(value);
    require(
        bundle::validateComponentBundleSnapshotIdentity(value),
        "coordinator snapshot should be canonical");
    return value;
}

struct Fixture
{
    bundle::ComponentOwnerAuthorizationStore store;
    probe::ComponentBundleProbeMacOSCoordinatorInputsV1 inputs;
    bundle::OwnedComponentApprovalFingerprint candidate;

    Fixture()
    {
        inputs.snapshot = snapshot();
        ComponentSignatureEvidenceView signature;
        signature.state = ComponentSignatureState::Verified;
        signature.contentDigest = inputs.snapshot.contentDigest;
        signature.signerIdentityDigest.fill(0x42);
        signature.profileVersion = currentComponentSignatureProfileVersion;
        signature.hasContentDigest = true;
        signature.hasSignerIdentity = true;
        inputs.signature =
            bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
        candidate.bundleId = inputs.snapshot.manifest.bundleId;
        candidate.semanticVersion = inputs.snapshot.manifest.semanticVersion;
        candidate.contentDigest = inputs.snapshot.contentDigest;
        candidate.signerIdentityDigest = signature.signerIdentityDigest;
        candidate.requestedPermissions =
            inputs.snapshot.manifest.requestedPermissions;
        candidate.policyVersion = currentComponentTrustPolicyVersion;
        candidate.hasContentDigest = true;
        candidate.hasSignerIdentity = true;
        const auto approved = store.recordOwnerAction(
            candidate.view(),
            bundle::ComponentOwnerAuthorizationAction::Approve,
            store.generation());
        require(approved.ok(), "coordinator owner approval should apply");
        inputs.ownerAuthorization = store.authorizationEvidence();
    }
};

void waitForTerminal(probe::ComponentBundleProbeMacOSCoordinator& coordinator)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (coordinator.operationPending()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!coordinator.operationPending(), "coordinator should settle");
}

void testPreparationOwnsAndBindsExactStepTwoAuthority()
{
    Fixture fixture;
    auto prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(fixture.inputs),
        probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
            TestShort);
    require(
        prepared.coordinator != nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        Ready
            && prepared.trustDecision
                == ComponentTrustDecision{
                    ComponentTrustDisposition::EligibleForProbe,
                    ComponentTrustDiagnosticCode::ProbeRequired}
            && prepared.coordinator->state()
                == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Prepared
            && prepared.coordinator->artifactGeneration() != 0,
        "exact snapshot, signature, and live approval should prepare once");
    const auto nonce = prepared.coordinator->validationNonce();
    require(
        std::ranges::any_of(nonce, [](auto byte) { return byte != 0; }),
        "prepared coordinator should own a secure nonzero nonce");

    Fixture secondFixture;
    auto second = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(secondFixture.inputs),
        probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
            TestShort);
    require(
        second.coordinator != nullptr
            && second.coordinator->artifactGeneration()
                != prepared.coordinator->artifactGeneration()
            && second.coordinator->validationNonce() != nonce,
        "each prepared coordinator should own a distinct generation and nonce");
}

void testInvalidSnapshotSignatureAndApprovalFailBeforeTransport()
{
    Fixture invalidSnapshot;
    invalidSnapshot.inputs.snapshot.contentDigest.front() ^= 0xffU;
    auto prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(invalidSnapshot.inputs));
    require(
        prepared.coordinator == nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        SnapshotInvalid,
        "tampered snapshot should fail before XPC");

    Fixture invalidSignature;
    auto signature = invalidSignature.inputs.signature.inspect();
    signature.contentDigest.front() ^= 0xffU;
    invalidSignature.inputs.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(invalidSignature.inputs));
    require(
        prepared.coordinator == nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        PolicyIneligible,
        "signature/snapshot mismatch should fail before XPC");

    Fixture wrongSigner;
    signature = wrongSigner.inputs.signature.inspect();
    signature.signerIdentityDigest.front() ^= 0xffU;
    wrongSigner.inputs.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(wrongSigner.inputs));
    require(
        prepared.coordinator == nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        PolicyIneligible
            && prepared.trustDecision
                == ComponentTrustDecision{
                    ComponentTrustDisposition::PreserveOnly,
                    ComponentTrustDiagnosticCode::ApprovalMissing},
        "approval for another signer should fail before XPC");

    Fixture missingApproval;
    const auto revoked = missingApproval.store.revokeOwnerAction(
        missingApproval.candidate.view(),
        missingApproval.store.generation());
    require(revoked.ok(), "test approval should revoke");
    prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(missingApproval.inputs));
    require(
        prepared.coordinator == nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        PolicyIneligible
            && prepared.trustDecision
                == ComponentTrustDecision{
                    ComponentTrustDisposition::PreserveOnly,
                    ComponentTrustDiagnosticCode::ApprovalMissing},
        "missing live approval should remain preserve-only");

    Fixture denied;
    const auto denial = denied.store.replaceOwnerAction(
        denied.candidate.view(),
        bundle::ComponentOwnerAuthorizationAction::Deny,
        denied.store.generation());
    require(denial.ok(), "test approval should replace with an exact denial");
    prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(denied.inputs));
    require(
        prepared.coordinator == nullptr
            && prepared.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        PolicyIneligible
            && prepared.trustDecision
                == ComponentTrustDecision{
                    ComponentTrustDisposition::Reject,
                    ComponentTrustDiagnosticCode::ApprovalDenied},
        "exact owner denial should reject before XPC");
}

void testRevocationCancellationAndConcurrentStartFailClosed()
{
    Fixture revoked;
    auto prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(revoked.inputs),
        probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
            TestShort);
    require(prepared.coordinator != nullptr, "revocation fixture should prepare");
    const auto removed = revoked.store.revokeOwnerAction(
        revoked.candidate.view(),
        revoked.store.generation());
    require(removed.ok(), "prepared approval should revoke live");
    require(
        prepared.coordinator->start()
            == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled,
        "first coordinator start should schedule");
    waitForTerminal(*prepared.coordinator);
    require(
        prepared.coordinator->state()
                == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Failed
            && prepared.coordinator->diagnostic()
                == probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    PolicyChanged
            && prepared.coordinator->evidence().inspect().state
                == ComponentProbeState::NotRun,
        "live revocation before transport should dominate probing");

    Fixture concurrent;
    prepared = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(concurrent.inputs),
        probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
            TestShort);
    require(
        prepared.coordinator->start()
                == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                    Scheduled
            && prepared.coordinator->start()
                == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                    InvalidState,
        "one coordinator should admit only one request");
    require(
        prepared.coordinator->cancel()
            == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled,
        "running coordinator should accept cancellation");
    waitForTerminal(*prepared.coordinator);
    require(
        prepared.coordinator->state()
                == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Cancelled
            && prepared.coordinator->diagnostic()
                == probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    Cancelled
            && !prepared.coordinator->evidence().inspect().authored,
        "cancellation should publish no terminal probe authority");
}
}

int main()
{
    testPreparationOwnsAndBindsExactStepTwoAuthority();
    testInvalidSnapshotSignatureAndApprovalFailBeforeTransport();
    testRevocationCancellationAndConcurrentStartFailClosed();
    return 0;
}
