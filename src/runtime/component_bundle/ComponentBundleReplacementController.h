#pragma once

#include "runtime/component_activation/macos/ComponentActivationMacOSHost.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleReplacementConfirmation.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.h"

#include "talas/project/ProjectDocument.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace talas::runtime::component_bundle
{
enum class ComponentBundleReplacementControllerOperation : std::uint8_t
{
    Scheduled,
    InvalidState,
    Rejected,
};

enum class ComponentBundleReplacementControllerState : std::uint8_t
{
    Idle,
    Inspecting,
    AwaitingOwnerApproval,
    AwaitingConfirmation,
    Probing,
    Activating,
    Staging,
    Succeeded,
    Failed,
    Cancelled,
};

struct ComponentBundleReplacementCandidateProjection
{
    std::string bundleId;
    std::string semanticVersion;
    std::string publisher;
    std::string licenseExpression;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    bool available = false;
};

enum class ComponentBundleReplacementControllerDiagnostic : std::uint8_t
{
    Ready,
    Inspecting,
    CandidateRejected,
    SignatureRejected,
    OwnerApprovalRequired,
    PreviewRejected,
    AwaitingConfirmation,
    ProbeRejected,
    ActivationRejected,
    StageRejected,
    Succeeded,
    Cancelled,
    InvalidConfiguration,
    AllocationFailed,
    InternalFailure,
};

struct ComponentBundleReplacementControllerInputs
{
    // Both pointed-to owners must outlive the controller. Workbench teardown
    // cancels and destroys the controller before replacing either owner.
    ProjectRuntimeBridge* bridge = nullptr;
    ComponentOwnerAuthorizationStore* ownerAuthorization = nullptr;
    talas::project::ProjectDocument document;
    RenderSettings renderSettings;
    std::uint64_t documentGeneration = 0;
    std::weak_ptr<const std::atomic<std::uint64_t>> liveDocumentGeneration;
    std::string nodeId;
    std::vector<ComponentBundleReplacementResourceTransition>
        resourceTransitions;
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
    std::function<ComponentBundleSignatureVerificationResult(
        const ComponentBundleSnapshot&)>
        signatureVerifierForTesting;
    component_probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1
        probeDeadlineProfileForTesting =
            component_probe::
                ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
                    Production;
    component_activation::ComponentActivationMacOSEndpointV1
        activationEndpointForTesting =
            component_activation::ComponentActivationMacOSEndpointV1::
                Production;
#endif
};

struct ComponentBundleReplacementControllerSnapshot
{
    ComponentBundleReplacementControllerState state =
        ComponentBundleReplacementControllerState::Idle;
    ComponentBundleReplacementControllerDiagnostic diagnostic =
        ComponentBundleReplacementControllerDiagnostic::Ready;
    std::uint64_t operationGeneration = 0;
    std::uint64_t previewGeneration = 0;
    ComponentBundleReplacementCandidateProjection candidate;
    std::shared_ptr<const ComponentBundleReplacementPreview> preview;
    ProjectStageOutcome stageOutcome = ProjectStageOutcome::CompileFailed;
    std::string message;
};

struct ComponentBundleReplacementControllerPreparation
{
    std::unique_ptr<class ComponentBundleReplacementController> controller;
    ComponentBundleReplacementControllerDiagnostic diagnostic =
        ComponentBundleReplacementControllerDiagnostic::
            InvalidConfiguration;

    [[nodiscard]] bool ok() const noexcept
    {
        return controller != nullptr
            && diagnostic
                == ComponentBundleReplacementControllerDiagnostic::Ready;
    }
};

class ComponentBundleReplacementController
{
public:
    ~ComponentBundleReplacementController();

    ComponentBundleReplacementController(
        const ComponentBundleReplacementController&) = delete;
    ComponentBundleReplacementController& operator=(
        const ComponentBundleReplacementController&) = delete;
    ComponentBundleReplacementController(
        ComponentBundleReplacementController&&) = delete;
    ComponentBundleReplacementController& operator=(
        ComponentBundleReplacementController&&) = delete;

    [[nodiscard]] static ComponentBundleReplacementControllerPreparation
    prepare(ComponentBundleReplacementControllerInputs inputs) noexcept;

    // Control-thread lifecycle methods. Inspection, Probe, and Runtime work is
    // asynchronous; stage() is the only method that enters ProjectRuntimeBridge.
    [[nodiscard]] ComponentBundleReplacementControllerOperation
    inspect(std::filesystem::path selectedDirectory) noexcept;
    [[nodiscard]] ComponentBundleReplacementControllerOperation
    approveCandidate() noexcept;
    [[nodiscard]] ComponentBundleReplacementControllerOperation
    confirm(std::uint64_t previewGeneration) noexcept;
    // Control-thread adoption point. The worker publishes Staging only after
    // inspection, Probe XPC, Runtime XPC, and exact binding preparation have
    // completed off-thread.
    [[nodiscard]] ComponentBundleReplacementControllerOperation stage()
        noexcept;
    [[nodiscard]] ComponentBundleReplacementControllerOperation cancel()
        noexcept;

    [[nodiscard]] ComponentBundleReplacementControllerSnapshot snapshot()
        const;

private:
    struct Impl;
    explicit ComponentBundleReplacementController(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> impl_;
};
}
