#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <unistd.h>

#ifndef TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_MISSING_SYMBOL_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_MISSING_SYMBOL_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_METADATA_MISMATCH_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_METADATA_MISMATCH_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_CONSTRUCTOR_CRASH_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CONSTRUCTOR_CRASH_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_ENTRY_CRASH_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_ENTRY_CRASH_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_ENTRY_HANG_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_ENTRY_HANG_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_UNLOAD_CRASH_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_UNLOAD_CRASH_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_UNLOAD_HANG_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_UNLOAD_HANG_FIXTURE_PATH is required
#endif
#ifndef TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO
#error TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO is required
#endif

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

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.good(), "coordinator fixture should be readable");
    const auto end = stream.tellg();
    require(end > 0, "coordinator fixture should contain bytes");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "coordinator fixture should read exactly");
    return bytes;
}

std::array<std::uint8_t, componentTrustDigestBytes> digest(
    std::span<const std::uint8_t> bytes)
{
    std::array<std::uint8_t, componentTrustDigestBytes> value{};
    require(
        !bytes.empty() && bytes.size() <= std::numeric_limits<CC_LONG>::max()
            && CC_SHA256(
                   bytes.data(),
                   static_cast<CC_LONG>(bytes.size()),
                   value.data()) != nullptr,
        "coordinator fixture digest should compute");
    return value;
}

bundle::ComponentBundleSnapshot snapshotFor(
    std::vector<std::uint8_t> entryBytes)
{
    bundle::ComponentBundleSnapshot snapshot;
    auto& manifest = snapshot.manifest;
    manifest.bundleId = "org.talas.fixture";
    manifest.semanticVersion = "1.2.3";
    manifest.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    manifest.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    manifest.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    manifest.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    manifest.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    manifest.entry.path = "Payload/entry.bundle";
    manifest.entry.byteCount = entryBytes.size();
    manifest.entry.sha256 = digest(entryBytes);
    bundle::ComponentBundleNode node;
    node.typeId = "org.talas.fixture.oscillator";
    manifest.nodes.push_back(std::move(node));
    manifest.state.currentSchemaVersion = 1;
    manifest.provenance.publisher = "Talas tests";
    manifest.provenance.licenseExpression = "Apache-2.0";
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        manifest);
    require(canonical.ok(), "coordinator manifest should canonicalize");
    snapshot.canonicalManifest = canonical.canonicalJson;
    bundle::ComponentBundleFrozenPayload entry;
    entry.path = manifest.entry.path;
    entry.sha256 = manifest.entry.sha256;
    entry.bytes = std::move(entryBytes);
    entry.isEntry = true;
    snapshot.payloads.push_back(std::move(entry));
    snapshot.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    require(
        bundle::validateComponentBundleSnapshotIdentity(snapshot),
        "coordinator fixture snapshot should be exact");
    return snapshot;
}

struct PreparedFixture
{
    bundle::ComponentOwnerAuthorizationStore store;
    bundle::OwnedComponentApprovalFingerprint candidate;
    probe::ComponentBundleProbeMacOSCoordinatorPreparationV1 preparation;
};

std::unique_ptr<PreparedFixture> prepare(
    std::vector<std::uint8_t> entryBytes)
{
    auto fixture = std::make_unique<PreparedFixture>();
    probe::ComponentBundleProbeMacOSCoordinatorInputsV1 inputs;
    inputs.snapshot = snapshotFor(std::move(entryBytes));
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = inputs.snapshot.contentDigest;
    signature.signerIdentityDigest.fill(0x62);
    signature.profileVersion = currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    inputs.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    fixture->candidate.bundleId = inputs.snapshot.manifest.bundleId;
    fixture->candidate.semanticVersion = inputs.snapshot.manifest.semanticVersion;
    fixture->candidate.contentDigest = inputs.snapshot.contentDigest;
    fixture->candidate.signerIdentityDigest = signature.signerIdentityDigest;
    fixture->candidate.requestedPermissions =
        inputs.snapshot.manifest.requestedPermissions;
    fixture->candidate.policyVersion = currentComponentTrustPolicyVersion;
    fixture->candidate.hasContentDigest = true;
    fixture->candidate.hasSignerIdentity = true;
    const auto approved = fixture->store.recordOwnerAction(
        fixture->candidate.view(),
        bundle::ComponentOwnerAuthorizationAction::Approve,
        fixture->store.generation());
    require(approved.ok(), "coordinator integration approval should apply");
    inputs.ownerAuthorization = fixture->store.authorizationEvidence();
    fixture->preparation = probe::ComponentBundleProbeMacOSCoordinator::prepare(
        std::move(inputs),
        probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
            TestShort);
    require(
        fixture->preparation.coordinator != nullptr,
        "coordinator integration fixture should prepare");
    return fixture;
}

[[maybe_unused]] void run(PreparedFixture& fixture)
{
    require(
        fixture.preparation.coordinator->start()
            == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled,
        "coordinator integration request should schedule");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (fixture.preparation.coordinator->operationPending()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(
        !fixture.preparation.coordinator->operationPending(),
        "coordinator integration request should terminate");
    const auto process = fixture.preparation.coordinator->processIdentifier();
    if (process > 0)
    {
        const auto exitDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < exitDeadline)
        {
            errno = 0;
            if (kill(process, 0) != 0 && errno == ESRCH)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(false, "disposable coordinator helper should exit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

[[maybe_unused]] void waitForCoordinator(PreparedFixture& fixture)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (fixture.preparation.coordinator->operationPending()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(
        !fixture.preparation.coordinator->operationPending(),
        "coordinator lifecycle fixture should terminate");
}

[[maybe_unused]] void requireFinished(
    PreparedFixture& fixture,
    ComponentProbeState state,
    probe::ComponentBundleProbeResultDiagnosticV1 resultDiagnostic)
{
    run(fixture);
    const auto view = fixture.preparation.coordinator->evidence().inspect();
    if (fixture.preparation.coordinator->state()
            != probe::ComponentBundleProbeMacOSCoordinatorStateV1::Finished
        || !view.authored || !view.sessionActive || view.state != state
        || fixture.preparation.coordinator->resultDiagnostic()
            != resultDiagnostic)
    {
        std::cerr << "state="
                  << static_cast<int>(
                         fixture.preparation.coordinator->state())
                  << " diagnostic="
                  << static_cast<int>(
                         fixture.preparation.coordinator->diagnostic())
                  << " result="
                  << static_cast<int>(
                         fixture.preparation.coordinator->resultDiagnostic())
                  << " authored=" << view.authored
                  << " active=" << view.sessionActive
                  << " probe-state=" << static_cast<int>(view.state)
                  << " expected-result=" << static_cast<int>(resultDiagnostic)
                  << '\n';
    }
    require(
        fixture.preparation.coordinator->state()
                == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Finished
            && view.authored && view.sessionActive && view.state == state
            && fixture.preparation.coordinator->resultDiagnostic()
                == resultDiagnostic,
        "coordinator should publish the expected validated terminal result");
}

[[maybe_unused]] void requireParentFailure(
    PreparedFixture& fixture,
    ComponentProbeState state,
    probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1 diagnostic)
{
    run(fixture);
    const auto view = fixture.preparation.coordinator->evidence().inspect();
    if (fixture.preparation.coordinator->state()
            != probe::ComponentBundleProbeMacOSCoordinatorStateV1::Failed
        || fixture.preparation.coordinator->diagnostic() != diagnostic
        || !view.authored || !view.sessionActive || view.state != state)
    {
        std::cerr << "parent-state="
                  << static_cast<int>(
                         fixture.preparation.coordinator->state())
                  << " diagnostic="
                  << static_cast<int>(
                         fixture.preparation.coordinator->diagnostic())
                  << " authored=" << view.authored
                  << " active=" << view.sessionActive
                  << " probe-state=" << static_cast<int>(view.state)
                  << " expected-diagnostic=" << static_cast<int>(diagnostic)
                  << '\n';
    }
    require(
        fixture.preparation.coordinator->state()
                == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Failed
            && fixture.preparation.coordinator->diagnostic() == diagnostic
            && view.authored && view.sessionActive && view.state == state,
        "coordinator should publish the expected parent-classified failure");
}

void runScenario()
{
    if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 1)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireFinished(
            *fixture,
            ComponentProbeState::Clean,
            probe::ComponentBundleProbeResultDiagnosticV1::Ready);
        auto retained = fixture->preparation.coordinator->evidence();
        fixture->preparation.coordinator.reset();
        require(
            retained.inspect().authored && !retained.inspect().sessionActive,
            "coordinator destruction should revoke copied clean evidence");
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 2)
    {
        auto bytes = readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH);
        bytes.front() ^= 0xffU;
        auto fixture = prepare(std::move(bytes));
        requireFinished(
            *fixture,
            ComponentProbeState::Rejected,
            probe::ComponentBundleProbeResultDiagnosticV1::EntryRejected);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 3)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_MISSING_SYMBOL_FIXTURE_PATH));
        requireFinished(
            *fixture,
            ComponentProbeState::Rejected,
            probe::ComponentBundleProbeResultDiagnosticV1::EntrySymbolMissing);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 4)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_METADATA_MISMATCH_FIXTURE_PATH));
        requireFinished(
            *fixture,
            ComponentProbeState::Rejected,
            probe::ComponentBundleProbeResultDiagnosticV1::AbiRejected);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 5)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH));
        requireFinished(
            *fixture,
            ComponentProbeState::ControlledFatal,
            probe::ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 6)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_CONSTRUCTOR_CRASH_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::AbnormalTermination,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::ProcessCrashed);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 7)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_ENTRY_CRASH_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::AbnormalTermination,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::ProcessCrashed);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 8)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_ENTRY_HANG_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::TimedOut,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::DeadlineExpired);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 9)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_UNLOAD_CRASH_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::AbnormalTermination,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::ProcessCrashed);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 10)
    {
        auto fixture = prepare(
            readBytes(TALAS_COMPONENT_PROBE_UNLOAD_HANG_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::TimedOut,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::DeadlineExpired);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 11)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::ProtocolError,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProtocolRejected);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 12)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::AbnormalTermination,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                LaunchDenied);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 13)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        require(
            fixture->preparation.coordinator->start()
                == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                    Scheduled,
            "revocation fixture should start");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto revoked = fixture->store.revokeOwnerAction(
            fixture->candidate.view(),
            fixture->store.generation());
        require(revoked.ok(), "in-flight approval should revoke");
        waitForCoordinator(*fixture);
        require(
            fixture->preparation.coordinator->state()
                    == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Failed
                && fixture->preparation.coordinator->diagnostic()
                    == probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                        PolicyChanged
                && !fixture->preparation.coordinator->evidence().inspect().authored,
            "revocation before a clean reply must dominate publication");
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 14)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        require(
            fixture->preparation.coordinator->start()
                    == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                        Scheduled
                && fixture->preparation.coordinator->cancel()
                    == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                        Scheduled,
            "in-flight delayed probe should cancel once");
        waitForCoordinator(*fixture);
        require(
            fixture->preparation.coordinator->state()
                    == probe::ComponentBundleProbeMacOSCoordinatorStateV1::
                        Cancelled
                && !fixture->preparation.coordinator->evidence().inspect().authored,
            "cancelled delayed reply must publish no authority");
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 15)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        require(
            fixture->preparation.coordinator->start()
                == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                    Scheduled,
            "destruction fixture should start");
        fixture->preparation.coordinator.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 16)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::TimedOut,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                DeadlineExpired);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 17)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::ProtocolError,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProtocolRejected);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 18)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::ProtocolError,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProtocolRejected);
    }
    else if constexpr (TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO == 19)
    {
        auto fixture = prepare(readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
        requireParentFailure(
            *fixture,
            ComponentProbeState::AbnormalTermination,
            probe::ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProcessCrashed);
    }
    else
    {
        require(false, "unknown coordinator integration scenario");
    }
}
}

int main()
{
    runScenario();
    return 0;
}
