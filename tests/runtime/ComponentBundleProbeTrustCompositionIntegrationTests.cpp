#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundlePreservationCompatibility.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/core/NodeCatalogJson.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "talas/runtime/ComponentIsolationMacOSHost.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH is required
#endif

extern char** environ;

namespace
{
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

constexpr std::string_view componentTypeId =
    "org.talas.fixture.oscillator";

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-probe-trust-composition-"
                   + std::to_string(::getpid()) + "-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "composition fixture should be readable");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void writeBytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "composition fixture should write exact bytes");
}

void runOpenSsl(const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>("/usr/bin/openssl"));
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t process = 0;
    require(
        ::posix_spawn(
            &process,
            "/usr/bin/openssl",
            nullptr,
            nullptr,
            argv.data(),
            environ)
            == 0,
        "composition fixture should launch OpenSSL");
    int status = 0;
    require(
        ::waitpid(process, &status, 0) == process && WIFEXITED(status)
            && WEXITSTATUS(status) == 0,
        "composition fixture OpenSSL command should succeed");
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
                   value.data())
                != nullptr,
        "composition fixture digest should compute");
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
    node.typeId = std::string(componentTypeId);
    node.stateSchemaVersion = 1;
    manifest.nodes.push_back(std::move(node));
    manifest.state.currentSchemaVersion = 1;
    manifest.provenance.publisher = "Talas tests";
    manifest.provenance.licenseExpression = "Apache-2.0";
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        manifest);
    require(canonical.ok(), "composition manifest should canonicalize");
    snapshot.canonicalManifest = canonical.canonicalJson;
    snapshot.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        manifest.entry.path,
        manifest.entry.sha256,
        std::move(entryBytes),
        true,
    });
    snapshot.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    require(
        bundle::validateComponentBundleSnapshotIdentity(snapshot),
        "composition snapshot should be exact");
    return snapshot;
}

ComponentSignatureEvidence signAndVerify(
    bundle::ComponentBundleSnapshot& snapshot,
    const TemporaryDirectory& directory)
{
    const auto message = bundle::makeComponentBundleSignatureMessage(
        snapshot.contentDigest);
    const auto messagePath = directory.path() / "message.bin";
    const auto certificatePem = directory.path() / "certificate.pem";
    const auto certificateDer = directory.path() / "certificate.der";
    const auto privateKey = directory.path() / "private-key.pem";
    const auto signaturePath = directory.path() / "signature.der";
    writeBytes(messagePath, message);
    runOpenSsl({
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-sha256",
        "-nodes",
        "-subj",
        "/CN=Talas Probe Trust Composition",
        "-keyout",
        privateKey.string(),
        "-out",
        certificatePem.string(),
        "-days",
        "3650",
    });
    runOpenSsl({
        "x509",
        "-in",
        certificatePem.string(),
        "-outform",
        "DER",
        "-out",
        certificateDer.string(),
    });
    runOpenSsl({
        "cms",
        "-sign",
        "-binary",
        "-in",
        messagePath.string(),
        "-signer",
        certificatePem.string(),
        "-inkey",
        privateKey.string(),
        "-outform",
        "DER",
        "-out",
        signaturePath.string(),
        "-nosmimecap",
    });
    snapshot.detachedSignatureCms = readBytes(signaturePath);
    const auto verified =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            snapshot,
            readBytes(certificateDer));
    require(
        verified.ok(),
        "composition should begin with one cryptographically verified exact signature");
    return verified.evidence;
}

talas::project::ProjectDocument preservedDocument()
{
    talas::project::ProjectPreservedComponent preserved;
    preserved.typeVersion = "1.2.3";
    preserved.stateSchemaVersion = 1;
    preserved.state = {0x4b, 0x43};
    preserved.displayName = "Probe-only oscillator";
    preserved.bundleId = "org.talas.fixture";
    preserved.recoveryHint = "Keep the original component capsule";

    talas::project::ProjectDocument document;
    talas::project::ProjectNode node;
    node.instanceId = "preserved-probe-node";
    node.typeId = std::string(componentTypeId);
    node.preservedComponent = std::move(preserved);
    document.graph.nodes.push_back(std::move(node));
    return document;
}

ComponentIsolationControlSessionConfigurationV1 isolationConfiguration()
{
    ComponentIsolationControlSessionConfigurationV1 value;
    for (std::size_t index = 0;
         index < value.contract.validationNonce.size();
         ++index)
    {
        value.contract.validationNonce[index] =
            static_cast<std::uint8_t>(index + 41U);
    }
    value.contract.sessionGeneration = 601;
    value.contract.sharedStorageIdentity = 701;
    value.contract.inputChannelCount = 2;
    value.contract.outputChannelCount = 2;
    value.contract.maximumFrames = 257;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.controlPayloadCapacityBytes = 128;
    return value;
}

ComponentIsolationMacOSHostPreparation prepareIsolationHost()
{
    const auto control = isolationConfiguration();
    ComponentIsolationAudioDataPlaneConfiguration audio;
    audio.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(audio);
    require(
        preparedAudio.diagnostic
                == ComponentIsolationAudioPreparationDiagnostic::Ready
            && preparedAudio.dataPlane != nullptr,
        "composition should prepare the host-owned isolation data plane");
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    return ComponentIsolationMacOSHost::prepare(
        control,
        std::move(sharedAudio),
        ComponentIsolationMacOSProductEndpointV1::ProductionGain);
}

bool waitForProbe(probe::ComponentBundleProbeMacOSCoordinator& coordinator)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (coordinator.operationPending()
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !coordinator.operationPending()
        && coordinator.state()
            == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Finished;
}

bool waitForIsolationState(
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
        {
            return true;
        }
        if (host.state() == ComponentIsolationControlSessionState::Quarantined)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testRealProbeAndIsolationComposeWithoutProductActivation()
{
    const auto catalogBefore = talas::core::serializeNodeCatalog(
        talas::core::makeBuiltinNodeRegistry());
    const auto registryCountBefore =
        talas::core::makeBuiltinNodeRegistry().nodeTypes().size();
    auto document = preservedDocument();
    const auto projectBefore = talas::project::serializeProjectDocument(document);
    const auto capsuleBefore =
        *document.graph.nodes.front().preservedComponent;

    TemporaryDirectory signingDirectory;
    auto snapshot = snapshotFor(
        readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH));
    const auto signature = signAndVerify(snapshot, signingDirectory);
    const auto signatureView = signature.inspect();

    bundle::OwnedComponentApprovalFingerprint candidate;
    candidate.bundleId = snapshot.manifest.bundleId;
    candidate.semanticVersion = snapshot.manifest.semanticVersion;
    candidate.contentDigest = snapshot.contentDigest;
    candidate.signerIdentityDigest = signatureView.signerIdentityDigest;
    candidate.requestedPermissions =
        snapshot.manifest.requestedPermissions;
    candidate.policyVersion = currentComponentTrustPolicyVersion;
    candidate.hasContentDigest = true;
    candidate.hasSignerIdentity = true;

    bundle::ComponentOwnerAuthorizationStore authorization;
    const auto approved = authorization.recordOwnerAction(
        candidate.view(),
        bundle::ComponentOwnerAuthorizationAction::Approve,
        authorization.generation());
    require(approved.ok(), "composition owner approval should be live");

    const auto snapshotBefore = snapshot;
    probe::ComponentBundleProbeMacOSCoordinatorInputsV1 probeInputs;
    probeInputs.snapshot = snapshot;
    probeInputs.signature = signature;
    probeInputs.ownerAuthorization = authorization.authorizationEvidence();
    auto preparedProbe =
        probe::ComponentBundleProbeMacOSCoordinator::prepare(
            std::move(probeInputs),
            probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
                TestShort);
    require(
        preparedProbe.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        Ready
            && preparedProbe.coordinator != nullptr,
        "exact approved candidate should prepare one disposable probe");

    auto preparedIsolation = prepareIsolationHost();
    require(
        preparedIsolation.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
            && preparedIsolation.host != nullptr,
        "product isolation backend should prepare independently");
    const auto probeStarted = preparedProbe.coordinator->start()
        == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled;
    const auto probeFinished = probeStarted
        && waitForProbe(*preparedProbe.coordinator);
    require(
        probeFinished,
        "original clean fixture should produce one live clean probe result; state="
            + std::to_string(static_cast<unsigned>(
                preparedProbe.coordinator->state()))
            + " diagnostic="
            + std::to_string(static_cast<unsigned>(
                preparedProbe.coordinator->diagnostic()))
            + " result="
            + std::to_string(static_cast<unsigned>(
                preparedProbe.coordinator->resultDiagnostic())));
    require(
        preparedIsolation.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForIsolationState(
                *preparedIsolation.host,
                ComponentIsolationControlSessionState::Ready),
        "product isolation helper should produce live containment evidence");

    ComponentTrustEvidence trust;
    trust.candidate = candidate.view();
    trust.ownerAuthorizationEvidence = authorization.authorizationEvidence();
    trust.signature = signature;
    trust.probe = preparedProbe.coordinator->evidence();
    trust.artifactGeneration =
        preparedProbe.coordinator->artifactGeneration();
    trust.validationNonce = preparedProbe.coordinator->validationNonce();
    trust.hasValidationNonce = true;
    trust.isolationEvidence = preparedIsolation.host->isolationEvidence();
    trust.declaredRoles = snapshot.manifest.declaredRoles;

    const auto decision = evaluateComponentTrust(trust);
    require(
        decision
            == ComponentTrustDecision{
                ComponentTrustDisposition::EligibleForIsolatedActivation,
                ComponentTrustDiagnosticCode::Ready},
        "all four live trust legs should reach only the closed eligible state");
    const auto projection = projectComponentTrust(decision);
    require(
        !projection.canExecute && !projection.canInstall,
        "eligible trust diagnostics must expose no run or install authority");

    const auto compatibility =
        bundle::projectPreservedComponentCompatibility(
            document.graph.nodes.front().typeId,
            *document.graph.nodes.front().preservedComponent,
            &snapshot,
            &trust);
    require(
        compatibility.code
                == bundle::ComponentBundlePreservationCode::
                    ActivationForbidden
            && !compatibility.executable(),
        "F5a preservation must reject activation even for complete live trust");

    const auto registryAfter = talas::core::makeBuiltinNodeRegistry();
    require(
        talas::core::serializeNodeCatalog(registryAfter) == catalogBefore
            && registryAfter.nodeTypes().size() == registryCountBefore
            && registryAfter.findNodeType(componentTypeId) == nullptr,
        "probing must not mutate registry count, catalog bytes, or visibility");
    require(
        talas::project::serializeProjectDocument(document) == projectBefore
            && *document.graph.nodes.front().preservedComponent
                == capsuleBefore
            && snapshot == snapshotBefore,
        "probing and trust projection must leave project and candidate bytes unchanged");

    require(
        preparedIsolation.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForIsolationState(
                *preparedIsolation.host,
                ComponentIsolationControlSessionState::Stopped),
        "composition fixture should stop the product helper cleanly");
}
}

int main()
{
    try
    {
        testRealProbeAndIsolationComposeWithoutProductActivation();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
