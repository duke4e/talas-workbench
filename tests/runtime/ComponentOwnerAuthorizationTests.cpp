#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
namespace bundle = talas::runtime::component_bundle;
using namespace talas::runtime;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

ComponentApprovalFingerprint fingerprint(
    std::string_view bundleId = "org.example.fixture",
    std::string_view version = "1.0.0")
{
    ComponentApprovalFingerprint value;
    value.bundleId = bundleId;
    value.semanticVersion = version;
    value.contentDigest.fill(0x11);
    value.signerIdentityDigest.fill(0x22);
    value.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio);
    value.policyVersion = currentComponentTrustPolicyVersion;
    value.hasContentDigest = true;
    value.hasSignerIdentity = true;
    return value;
}

ComponentTrustEvidence trustEvidence(
    const ComponentApprovalFingerprint& candidate,
    const bundle::ComponentOwnerAuthorizationStore& store)
{
    ComponentTrustEvidence evidence;
    evidence.candidate = candidate;
    evidence.ownerAuthorizationEvidence = store.authorizationEvidence();
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = candidate.contentDigest;
    signature.signerIdentityDigest = candidate.signerIdentityDigest;
    signature.profileVersion = currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    evidence.artifactGeneration = 1;
    evidence.validationNonce.fill(0x33);
    evidence.hasValidationNonce = true;
    evidence.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor);
    return evidence;
}

void testEvaluatorConsumesLiveStoreAuthoredAuthorizationEvidence()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto approved = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(approved.ok(), "approval fixture should apply");

    const auto evidence = trustEvidence(candidate, store);
    require(
        evaluateComponentTrust(evidence)
            == ComponentTrustDecision{
                ComponentTrustDisposition::EligibleForProbe,
                ComponentTrustDiagnosticCode::ProbeRequired},
        "store-authored exact approval should admit probing");

    const auto denied = store.replaceOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        store.generation());
    require(
        denied.ok()
            && evaluateComponentTrust(evidence)
                == ComponentTrustDecision{
                    ComponentTrustDisposition::Reject,
                    ComponentTrustDiagnosticCode::ApprovalDenied},
        "the same evidence must observe a later explicit denial");
    auto unsignedDenied = evidence;
    unsignedDenied.signature = {};
    require(
        evaluateComponentTrust(unsignedDenied)
            == ComponentTrustDecision{
                ComponentTrustDisposition::Reject,
                ComponentTrustDiagnosticCode::ApprovalDenied},
        "owner denial must dominate missing signature preservation");

    const auto revoked = store.revokeOwnerAction(
        candidate,
        store.generation());
    require(
        revoked.ok()
            && evaluateComponentTrust(evidence)
                == ComponentTrustDecision{
                    ComponentTrustDisposition::PreserveOnly,
                    ComponentTrustDiagnosticCode::ApprovalMissing},
        "the same evidence must not replay an approval after revocation");

    ComponentTrustEvidence expired;
    {
        bundle::ComponentOwnerAuthorizationStore ephemeral;
        const auto ephemeralApproval = ephemeral.recordOwnerAction(
            candidate,
            bundle::ComponentOwnerAuthorizationAction::Approve,
            ephemeral.generation());
        require(ephemeralApproval.ok(), "ephemeral approval should apply");
        expired = trustEvidence(candidate, ephemeral);
    }
    require(
        evaluateComponentTrust(expired)
            == ComponentTrustDecision{
                ComponentTrustDisposition::PreserveOnly,
                ComponentTrustDiagnosticCode::ApprovalMissing},
        "evidence must expire when its process-local owner store is destroyed");
}

void testExplicitApprovalReturnsAnOwnedExactEvaluatorRecord()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto initialGeneration = store.generation();
    const auto mutation = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        initialGeneration);
    require(
        mutation.ok() && mutation.generation == initialGeneration + 1U
            && store.size() == 1U,
        "an explicit owner approval should advance one store generation");

    const auto lookup = store.lookup(candidate);
    const auto storedView = lookup.record->view();
    require(
        lookup.approved() && lookup.generation == mutation.generation
            && lookup.record->bundleId == candidate.bundleId
            && matchComponentOwnerApproval(candidate, &storedView)
                    .ownerApproval
                == ComponentOwnerApprovalState::ExactMatch,
        "lookup should own and re-match the exact evaluator fingerprint");
}

void testEveryFingerprintMutationMissesTheExactSet()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto approved = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(approved.ok(), "approval fixture should apply");

    auto changed = candidate;
    changed.bundleId = "org.example.other";
    require(!store.lookup(changed).approved(), "bundle ID must be exact");
    changed = candidate;
    changed.semanticVersion = "1.0.1";
    require(!store.lookup(changed).approved(), "version must be exact");
    changed = candidate;
    changed.contentDigest.front() ^= 0xffU;
    require(!store.lookup(changed).approved(), "content digest must be exact");
    changed = candidate;
    changed.signerIdentityDigest.front() ^= 0xffU;
    require(!store.lookup(changed).approved(), "signer digest must be exact");
    changed = candidate;
    changed.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    require(!store.lookup(changed).approved(), "permissions must be exact");
    changed = candidate;
    ++changed.policyVersion;
    require(!store.lookup(changed).approved(), "policy version must be exact");
    changed = candidate;
    changed.hasContentDigest = false;
    require(
        store.lookup(changed).match.ownerApproval
            == ComponentOwnerApprovalState::Invalid,
        "malformed candidates should reject rather than miss");
    changed = candidate;
    changed.hasSignerIdentity = false;
    require(
        store.lookup(changed).match.ownerApproval
            == ComponentOwnerApprovalState::Invalid,
        "missing signer identity should reject rather than miss");
}

void testDenialReplacementRevocationAndDuplicateActionsAreExplicit()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto denied = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        store.generation());
    require(denied.ok() && store.lookup(candidate).denied(),
        "an exact owner denial should be stored separately");

    const auto duplicate = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        store.generation());
    require(
        duplicate.code
                == bundle::ComponentOwnerAuthorizationMutationCode::DuplicateAction
            && duplicate.generation == store.generation(),
        "duplicate denial should not advance generation");

    const auto implicitReplacement = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(
        implicitReplacement.code
            == bundle::ComponentOwnerAuthorizationMutationCode::ReplacementRequired,
        "opposite actions must use the explicit replacement API");

    const auto replaced = store.replaceOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(replaced.ok() && store.lookup(candidate).approved(),
        "explicit replacement should atomically switch denial to approval");

    const auto revoked = store.revokeOwnerAction(
        candidate,
        store.generation());
    require(
        revoked.ok()
            && store.lookup(candidate).match.ownerApproval
                == ComponentOwnerApprovalState::Missing
            && store.size() == 0U,
        "explicit revocation should remove the exact record");
    const auto missing = store.revokeOwnerAction(
        candidate,
        store.generation());
    require(
        missing.code
            == bundle::ComponentOwnerAuthorizationMutationCode::MissingRecord,
        "revoking an absent fingerprint should not change state");
}

void testStaleGenerationAndConcurrentActionsFailClosed()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto staleGeneration = store.generation();
    const auto first = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        staleGeneration);
    require(first.ok(), "first action should apply");

    const auto stale = store.revokeOwnerAction(candidate, staleGeneration);
    require(
        stale.code
                == bundle::ComponentOwnerAuthorizationMutationCode::StaleGeneration
            && store.lookup(candidate).approved(),
        "a stale UI generation must not revoke a newer decision");

    bundle::ComponentOwnerAuthorizationStore concurrent;
    const auto expectedGeneration = concurrent.generation();
    std::array<bundle::ComponentOwnerAuthorizationMutationCode, 8> codes{};
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < codes.size(); ++index)
    {
        workers.emplace_back([&, index] {
            codes[index] = concurrent.recordOwnerAction(
                candidate,
                bundle::ComponentOwnerAuthorizationAction::Approve,
                expectedGeneration)
                               .code;
        });
    }
    for (auto& worker : workers)
        worker.join();
    require(
        std::ranges::count(
            codes,
            bundle::ComponentOwnerAuthorizationMutationCode::Applied) == 1
            && std::ranges::count(
                codes,
                bundle::ComponentOwnerAuthorizationMutationCode::StaleGeneration)
                == 7,
        "one generation can admit exactly one concurrent owner mutation");
}

void testExactApprovalLeaseSerializesTerminalPublicationAndRevocation()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto approved = store.recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(approved.ok(), "approval lease fixture should apply");

    const auto expectedGeneration = store.generation();
    auto publicationLease =
        store.authorizationEvidence().acquireExactApprovalLease(candidate);
    require(
        publicationLease.approved(),
        "an exact live approval should issue a publication lease");

    std::atomic<bool> revokeStarted{false};
    std::atomic<bool> revokeCompleted{false};
    bundle::ComponentOwnerAuthorizationMutationCode revokeCode =
        bundle::ComponentOwnerAuthorizationMutationCode::InvalidFingerprint;
    std::thread revoker([&] {
        revokeStarted.store(true, std::memory_order_release);
        revokeCode = store.revokeOwnerAction(candidate, expectedGeneration).code;
        revokeCompleted.store(true, std::memory_order_release);
    });
    while (!revokeStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(
        !revokeCompleted.load(std::memory_order_acquire),
        "revocation must not complete inside terminal publication");

    publicationLease = {};
    revoker.join();
    require(
        revokeCompleted.load(std::memory_order_acquire)
            && revokeCode
                == bundle::ComponentOwnerAuthorizationMutationCode::Applied
            && !store.lookup(candidate).approved(),
        "revocation should apply immediately after terminal publication");
}

void testStoreDestructionSharesTheApprovalLeaseLinearizationDomain()
{
    auto store = std::make_unique<bundle::ComponentOwnerAuthorizationStore>();
    const auto candidate = fingerprint();
    const auto approved = store->recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store->generation());
    require(approved.ok(), "destruction lease fixture should apply");
    const auto evidence = store->authorizationEvidence();
    auto publicationLease = evidence.acquireExactApprovalLease(candidate);
    require(
        publicationLease.approved(),
        "live store should issue an exact publication lease");

    std::atomic<bool> destructionStarted{false};
    std::atomic<bool> destructionCompleted{false};
    std::thread destroyer([&] {
        destructionStarted.store(true, std::memory_order_release);
        store.reset();
        destructionCompleted.store(true, std::memory_order_release);
    });
    while (!destructionStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(
        !destructionCompleted.load(std::memory_order_acquire),
        "store destruction must not invalidate approval during publication");

    publicationLease = {};
    destroyer.join();
    require(
        destructionCompleted.load(std::memory_order_acquire)
            && evidence.inspect(candidate).ownerApproval
                == ComponentOwnerApprovalState::Missing
            && !evidence.acquireExactApprovalLease(candidate).approved(),
        "destroyed stores must invalidate evidence and future leases");
}

void testRejectedApprovalLeaseDoesNotRetainTheAuthorizationMutex()
{
    auto store = std::make_unique<bundle::ComponentOwnerAuthorizationStore>();
    const auto candidate = fingerprint();
    const auto denied = store->recordOwnerAction(
        candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny,
        store->generation());
    require(denied.ok(), "rejected lease fixture should apply denial");

    auto rejectedLease =
        store->authorizationEvidence().acquireExactApprovalLease(candidate);
    require(
        !rejectedLease.approved(),
        "a denied candidate must not receive an approval lease");

    std::atomic<bool> destructionStarted{false};
    std::atomic<bool> destructionCompleted{false};
    std::thread destroyer([&] {
        destructionStarted.store(true, std::memory_order_release);
        store.reset();
        destructionCompleted.store(true, std::memory_order_release);
    });
    while (!destructionStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(100);
    while (!destructionCompleted.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    const auto completedWhileRejectedLeaseExists =
        destructionCompleted.load(std::memory_order_acquire);
    rejectedLease = {};
    destroyer.join();
    require(
        completedWhileRejectedLeaseExists,
        "an unsuccessful lease must return without retaining the store mutex");
}

void testActivationUseRevokesWithoutHoldingTheAuthorizationMutex()
{
    static_assert(!std::is_copy_constructible_v<
        ComponentOwnerActivationUseV1>);
    static_assert(std::is_nothrow_move_constructible_v<
        ComponentOwnerActivationUseV1>);

    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    const auto other = fingerprint("org.example.other");
    require(
        store.recordOwnerAction(
                 candidate,
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 store.generation())
            .ok(),
        "activation-use approval should apply");

    auto use = store.authorizationEvidence().acquireExactActivationUse(
        candidate);
    require(
        use.active(),
        "an exact live approval should issue an active use token");

    require(
        store.recordOwnerAction(
                 other,
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 store.generation())
            .ok()
            && use.active(),
        "an unrelated owner mutation must not revoke the exact use token");

    require(
        store.replaceOwnerAction(
                 candidate,
                 bundle::ComponentOwnerAuthorizationAction::Deny,
                 store.generation())
            .ok()
            && !use.active(),
        "an exact denial must revoke existing activation-use tokens");
    require(
        !store.authorizationEvidence()
             .acquireExactActivationUse(candidate)
             .active(),
        "a denied candidate must not issue another activation-use token");
}

void testActivationUseRevokesOnRecordRevocationAndStoreDestruction()
{
    const auto candidate = fingerprint();
    ComponentOwnerActivationUseV1 destroyedUse;
    {
        auto store = std::make_unique<
            bundle::ComponentOwnerAuthorizationStore>();
        require(
            store->recordOwnerAction(
                     candidate,
                     bundle::ComponentOwnerAuthorizationAction::Approve,
                     store->generation())
                .ok(),
            "destruction activation-use approval should apply");
        destroyedUse = store->authorizationEvidence()
                           .acquireExactActivationUse(candidate);
        require(
            destroyedUse.active(),
            "live store should issue an activation-use token");
    }
    require(
        !destroyedUse.active(),
        "store destruction must revoke every retained activation-use token");

    bundle::ComponentOwnerAuthorizationStore store;
    require(
        store.recordOwnerAction(
                 candidate,
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 store.generation())
            .ok(),
        "revocation activation-use approval should apply");
    auto first = store.authorizationEvidence().acquireExactActivationUse(
        candidate);
    auto second = store.authorizationEvidence().acquireExactActivationUse(
        candidate);
    require(first.active() && second.active(),
        "one approval may back multiple exact prepared uses");
    require(
        store.revokeOwnerAction(candidate, store.generation()).ok()
            && !first.active() && !second.active(),
        "record revocation must revoke all exact prepared uses");
}

void testActivationUseCapacityIsBoundedAndReclaimsExpiredUses()
{
    bundle::ComponentOwnerAuthorizationStore store;
    const auto candidate = fingerprint();
    require(
        store.recordOwnerAction(
                 candidate,
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 store.generation())
            .ok(),
        "capacity activation-use approval should apply");

    std::vector<ComponentOwnerActivationUseV1> uses;
    uses.reserve(bundle::maximumConcurrentComponentActivationUses);
    for (std::size_t index = 0;
         index < bundle::maximumConcurrentComponentActivationUses;
         ++index)
    {
        auto use = store.authorizationEvidence().acquireExactActivationUse(
            candidate);
        require(
            use.active(),
            "every in-budget activation use should be active");
        uses.push_back(std::move(use));
    }
    require(
        !store.authorizationEvidence()
             .acquireExactActivationUse(candidate)
             .active(),
        "activation-use issuance must fail closed at its exact capacity");

    uses.pop_back();
    auto reclaimed = store.authorizationEvidence().acquireExactActivationUse(
        candidate);
    require(
        reclaimed.active(),
        "an expired activation use should release one bounded slot");

    require(
        store.revokeOwnerAction(candidate, store.generation()).ok()
            && std::ranges::none_of(uses, [](const auto& use) {
                   return use.active();
               })
            && !reclaimed.active(),
        "revocation should deactivate every retained capacity token");
    require(
        store.recordOwnerAction(
                 candidate,
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 store.generation())
            .ok(),
        "the owner should be able to approve the exact candidate again");
    require(
        store.authorizationEvidence()
            .acquireExactActivationUse(candidate)
            .active(),
        "inactive tokens must not consume capacity after reauthorization");
}

void testCapacityInvalidInputAndRestartEmptyBehavior()
{
    bundle::ComponentOwnerAuthorizationStore store;
    auto generation = store.generation();
    for (std::size_t index = 0;
         index < bundle::maximumEphemeralComponentOwnerAuthorizations;
         ++index)
    {
        const std::string bundleId =
            "org.example.fixture" + std::to_string(index);
        const auto result = store.recordOwnerAction(
            fingerprint(bundleId),
            bundle::ComponentOwnerAuthorizationAction::Approve,
            generation);
        require(result.ok(), "every in-budget exact record should apply");
        generation = result.generation;
    }
    const auto full = store.recordOwnerAction(
        fingerprint("org.example.overflow"),
        bundle::ComponentOwnerAuthorizationAction::Deny,
        generation);
    require(
        full.code
                == bundle::ComponentOwnerAuthorizationMutationCode::CapacityExceeded
            && full.generation == generation
            && store.size()
                == bundle::maximumEphemeralComponentOwnerAuthorizations,
        "authorization storage should fail closed at its exact capacity");

    auto malformed = fingerprint();
    malformed.policyVersion = 0;
    const auto invalid = store.recordOwnerAction(
        malformed,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        store.generation());
    require(
        invalid.code
            == bundle::ComponentOwnerAuthorizationMutationCode::InvalidFingerprint,
        "unvalidated fingerprints must never enter the store");

    const auto invalidAction = store.recordOwnerAction(
        fingerprint("org.example.invalid-action"),
        static_cast<bundle::ComponentOwnerAuthorizationAction>(0xff),
        store.generation());
    require(
        invalidAction.code
            == bundle::ComponentOwnerAuthorizationMutationCode::InvalidAction,
        "unknown owner actions must fail closed without mutation");

    bundle::ComponentOwnerAuthorizationStore restarted;
    require(
        restarted.size() == 0U
            && restarted.lookup(fingerprint()).match.ownerApproval
                == ComponentOwnerApprovalState::Missing,
        "a new process-local store must start empty without persistence");
}
}

int main()
{
    testEvaluatorConsumesLiveStoreAuthoredAuthorizationEvidence();
    testExplicitApprovalReturnsAnOwnedExactEvaluatorRecord();
    testEveryFingerprintMutationMissesTheExactSet();
    testDenialReplacementRevocationAndDuplicateActionsAreExplicit();
    testStaleGenerationAndConcurrentActionsFailClosed();
    testExactApprovalLeaseSerializesTerminalPublicationAndRevocation();
    testStoreDestructionSharesTheApprovalLeaseLinearizationDomain();
    testRejectedApprovalLeaseDoesNotRetainTheAuthorizationMutex();
    testActivationUseRevokesWithoutHoldingTheAuthorizationMutex();
    testActivationUseRevokesOnRecordRevocationAndStoreDestruction();
    testActivationUseCapacityIsBoundedAndReclaimsExpiredUses();
    testCapacityInvalidInputAndRestartEmptyBehavior();
    return 0;
}
