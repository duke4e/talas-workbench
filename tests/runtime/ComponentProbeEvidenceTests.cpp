#include "talas/runtime/ComponentTrustPolicy.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace
{
using namespace talas::runtime;
namespace probe = talas::runtime::component_probe;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

probe::ComponentBundleProbeRequestV1 validRequest()
{
    probe::ComponentBundleProbeRequestV1 request;
    request.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    constexpr std::string_view id = "org.talas.probe-evidence";
    constexpr std::string_view version = "1.2.3";
    request.bundleIdBytes = id.size();
    request.semanticVersionBytes = version.size();
    std::copy(id.begin(), id.end(), request.bundleId.begin());
    std::copy(version.begin(), version.end(), request.semanticVersion.begin());
    request.contentDigest.fill(0x31);
    request.signerIdentityDigest.fill(0x73);
    request.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    request.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    request.policyVersion = currentComponentTrustPolicyVersion;
    request.entryByteCount = 4096;
    request.artifactGeneration = 19;
    request.validationNonce.fill(0x5c);
    return request;
}

probe::ComponentBundleProbeResultV1 resultFor(
    const probe::ComponentBundleProbeRequestV1& request,
    ComponentProbeState state)
{
    probe::ComponentBundleProbeResultV1 result;
    result.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(state);
    result.diagnostic = static_cast<std::uint32_t>(
        state == ComponentProbeState::Clean
            ? probe::ComponentBundleProbeResultDiagnosticV1::Ready
            : state == ComponentProbeState::Rejected
            ? probe::ComponentBundleProbeResultDiagnosticV1::MetadataRejected
            : probe::ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    return result;
}

void testEvidenceIsOpaqueOwnedAndSessionBound()
{
    static_assert(!std::is_aggregate_v<ComponentProbeEvidence>);
    const auto request = validRequest();
    auto authored = probe::ComponentBundleProbeEvidenceFactory::
        makeFromValidatedResult(
            resultFor(request, ComponentProbeState::Clean), request);
    require(authored.has_value(), "complete clean evidence should author");
    auto evidence = authored->evidence;
    const auto active = evidence.inspect();
    require(
        active.authored && active.sessionActive
            && active.state == ComponentProbeState::Clean
            && active.candidateFingerprint.bundleId
                == "org.talas.probe-evidence",
        "opaque evidence should expose one read-only active view");

    const auto copied = evidence;
    authored->session.revoke();
    require(
        !evidence.inspect().sessionActive
            && !copied.inspect().sessionActive,
        "revocation should invalidate every copied evidence handle");
}

void testFactoryCopiesFingerprintAndExpiryIsVisible()
{
    ComponentProbeEvidence evidence;
    {
        auto request = validRequest();
        constexpr std::string_view bundleId = "org.talas.owned-probe";
        constexpr std::string_view version = "2.0.0-rc.1";
        request.bundleId.fill(0);
        request.semanticVersion.fill(0);
        request.bundleIdBytes = bundleId.size();
        request.semanticVersionBytes = version.size();
        std::copy(bundleId.begin(), bundleId.end(), request.bundleId.begin());
        std::copy(version.begin(), version.end(), request.semanticVersion.begin());
        auto authored = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(
                resultFor(request, ComponentProbeState::Rejected), request);
        require(authored.has_value(), "complete rejected evidence should author");
        evidence = authored->evidence;
        require(
            evidence.inspect().candidateFingerprint.bundleId == bundleId,
            "the factory should own candidate text independently");
    }
    const auto expired = evidence.inspect();
    require(
        expired.authored && !expired.sessionActive
            && expired.candidateFingerprint.bundleId
                == "org.talas.owned-probe",
        "expired evidence should retain safe diagnostics but no authority");
}

void testMalformedEvidenceCannotBeAuthored()
{
    require(
        !probe::ComponentBundleProbeEvidenceFactory::makeParentClassifiedFailure(
             ComponentProbeState::Clean, validRequest())
             .has_value(),
        "clean evidence cannot come from a parent classification");
    auto incomplete = validRequest();
    incomplete.validationNonce.fill(0);
    require(
        !probe::ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
             resultFor(incomplete, ComponentProbeState::Clean), incomplete)
             .has_value(),
        "an incomplete clean record cannot be authored");
    auto missingDigest = validRequest();
    missingDigest.contentDigest.fill(0);
    require(
        !probe::ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
             resultFor(missingDigest, ComponentProbeState::Clean),
             missingDigest)
             .has_value(),
        "a request without an artifact digest cannot be authored");
    const auto request = validRequest();
    auto mismatched = resultFor(request, ComponentProbeState::Clean);
    mismatched.contentDigest.front() ^= 0xff;
    require(
        !probe::ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
             mismatched, request)
             .has_value(),
        "a result for a different artifact cannot be authored");
    auto unknown = resultFor(request, ComponentProbeState::Clean);
    unknown.state = 0xff;
    require(
        !probe::ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
             unknown, request)
             .has_value(),
        "an unknown terminal state cannot be authored");

    auto parentFailure = probe::ComponentBundleProbeEvidenceFactory::
        makeParentClassifiedFailure(
            ComponentProbeState::TimedOut, request);
    require(
        parentFailure.has_value()
            && parentFailure->evidence.inspect().sessionActive
            && parentFailure->evidence.inspect().state
                == ComponentProbeState::TimedOut,
        "the parent may author one bounded terminal transport failure");
}

void testConcurrentInspectionAndRevocationAreCoherent()
{
    const auto request = validRequest();
    auto authored = probe::ComponentBundleProbeEvidenceFactory::
        makeFromValidatedResult(
            resultFor(request, ComponentProbeState::Clean), request);
    require(authored.has_value(), "concurrency fixture should author");
    const auto evidence = authored->evidence;
    std::atomic<bool> start{false};
    std::atomic<bool> coherent{true};
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (std::size_t index = 0; index < 10'000; ++index)
        {
            const auto view = evidence.inspect();
            if (!view.authored
                || view.candidateFingerprint.bundleId
                    != "org.talas.probe-evidence")
            {
                coherent.store(false, std::memory_order_release);
            }
        }
    });
    start.store(true, std::memory_order_release);
    authored->session.revoke();
    reader.join();
    require(
        coherent.load(std::memory_order_acquire)
            && !evidence.inspect().sessionActive,
        "inspection must stay coherent and revocation must dominate after join");
}

void testCleanProbeIssuesMoveOnlyLivenessThatRevokesWithItsSession()
{
    static_assert(!std::is_copy_constructible_v<ComponentProbeLivenessV1>);
    static_assert(
        std::is_nothrow_move_constructible_v<ComponentProbeLivenessV1>);

    const auto request = validRequest();
    ComponentProbeLivenessV1 retained;
    {
        auto authored = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(
                resultFor(request, ComponentProbeState::Clean), request);
        require(authored.has_value(), "clean liveness fixture should author");
        retained = authored->evidence.acquireLiveness();
        require(
            retained.active(),
            "active clean evidence should issue one lock-free liveness token");
    }
    require(
        !retained.active(),
        "destroying the validation session must revoke retained liveness");

    auto rejected = probe::ComponentBundleProbeEvidenceFactory::
        makeFromValidatedResult(
            resultFor(request, ComponentProbeState::Rejected), request);
    require(
        rejected.has_value()
            && !rejected->evidence.acquireLiveness().active(),
        "non-clean terminal evidence must not issue activation liveness");

    auto clean = probe::ComponentBundleProbeEvidenceFactory::
        makeFromValidatedResult(
            resultFor(request, ComponentProbeState::Clean), request);
    require(clean.has_value(), "explicit revoke fixture should author");
    auto live = clean->evidence.acquireLiveness();
    clean->session.revoke();
    require(
        !live.active(),
        "explicit probe revocation must dominate retained activation liveness");
}
}

int main()
{
    testEvidenceIsOpaqueOwnedAndSessionBound();
    testFactoryCopiesFingerprintAndExpiryIsVisible();
    testMalformedEvidenceCannotBeAuthored();
    testConcurrentInspectionAndRevocationAreCoherent();
    testCleanProbeIssuesMoveOnlyLivenessThatRevokesWithItsSession();
    return 0;
}
