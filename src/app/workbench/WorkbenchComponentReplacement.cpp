#include "app/workbench/WorkbenchComponentInternal.h"

#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
#include "runtime/component_bundle/ComponentBundleReplacementController.h"
#endif

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace talas::app::workbench
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
struct ComponentReplacementControllerState
{
    talas::runtime::component_bundle::ComponentOwnerAuthorizationStore
        ownerAuthorization;
    std::shared_ptr<std::atomic<std::uint64_t>> documentGeneration =
        std::make_shared<std::atomic<std::uint64_t>>(1);
    std::unique_ptr<
        talas::runtime::component_bundle::ComponentBundleReplacementController>
        controller;
    std::string nodeId;
    double analysisSampleRate = 0.0;
    talas::audio::ProjectAssetWatchSnapshot verifiedAssetWatchBaseline;
    bool productStageIntegrated = false;
    bool surfaceDismissed = false;
};

void ComponentReplacementControllerStateDeleter::operator()(
    ComponentReplacementControllerState* state) const noexcept
{
    delete state;
}
#endif

namespace
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
namespace replacement = talas::runtime::component_bundle;

[[nodiscard]] juce::String digestText(
    const std::array<
        std::uint8_t,
        talas::runtime::componentTrustDigestBytes>& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest)
        stream << std::setw(2) << static_cast<unsigned>(byte);
    return juceString(stream.str());
}

[[nodiscard]] juce::String resourceKindText(
    talas::core::ResourceSnapshotKind kind)
{
    switch (kind)
    {
    case talas::core::ResourceSnapshotKind::Table:
        return "Table";
    case talas::core::ResourceSnapshotKind::Binary:
        return "Binary";
    case talas::core::ResourceSnapshotKind::Audio:
        return "Audio";
    case talas::core::ResourceSnapshotKind::PartialSpectrum:
        return "Partial spectrum";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view portTypeText(talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio: return "audio";
    case talas::core::PortType::Control: return "control";
    case talas::core::PortType::Event: return "event";
    case talas::core::PortType::SampleBuffer: return "sample-buffer";
    case talas::core::PortType::Wavetable: return "wavetable";
    case talas::core::PortType::Spectrum: return "spectrum";
    case talas::core::PortType::ComplexSpectrum: return "complex-spectrum";
    case talas::core::PortType::NoteStream: return "note-stream";
    }
    return "unknown";
}

[[nodiscard]] std::string_view parameterKindText(
    talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression: return "expression";
    case talas::core::ParameterKind::Choice: return "choice";
    case talas::core::ParameterKind::Boolean: return "boolean";
    case talas::core::ParameterKind::Resource: return "resource";
    case talas::core::ParameterKind::Text: return "text";
    }
    return "unknown";
}

[[nodiscard]] std::string_view valueDomainText(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar: return "scalar";
    case talas::expr::ValueDomain::Hertz: return "hertz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::NoteNumber: return "note-number";
    case talas::expr::ValueDomain::Samples: return "samples";
    }
    return "unknown";
}

void appendPorts(
    juce::String& details,
    std::string_view heading,
    const std::vector<replacement::ComponentBundlePort>& ports)
{
    details += "\n" + juceString(heading) + " ("
        + juce::String(static_cast<juce::int64>(ports.size())) + "):";
    if (ports.empty())
    {
        details += " none";
        return;
    }
    for (const auto& port : ports)
    {
        details += "\n  " + juceString(port.id)
            + juceString(" · ") + juceString(portTypeText(port.type))
            + (port.required
                   ? juceString(" · required")
                   : juceString(" · optional"))
            + juceString(" · ") + juceString(talas::core::portSignalRateName(
                port.signal.rate))
            + juceString(" · ") + juceString(talas::core::audioBusLayoutKindName(
                port.signal.audioLayout))
            + juceString(" · ") + juceString(talas::core::portEdgeSemanticsName(
                port.signal.edge))
            + juceString(" · ") + juceString(talas::core::spectralFrameFormatName(
                port.signal.spectralFormat))
            + (port.signal.timelineBearing
                ? juceString(" · timeline-bearing")
                : juceString(" · not timeline-bearing"));
    }
}

void appendParameters(
    juce::String& details,
    const std::vector<replacement::ComponentBundleParameter>& parameters)
{
    details += "\nParameters ("
        + juce::String(static_cast<juce::int64>(parameters.size())) + "):";
    if (parameters.empty())
    {
        details += " none";
        return;
    }
    for (const auto& parameter : parameters)
    {
        details += "\n  " + juceString(parameter.id)
            + juceString(" · ")
            + juceString(parameterKindText(parameter.kind))
            + juceString(" · ")
            + juceString(valueDomainText(parameter.domain));
        if (!parameter.choices.empty())
        {
            details += juceString(" · choices [");
            for (std::size_t index = 0; index < parameter.choices.size(); ++index)
            {
                if (index != 0)
                    details += ", ";
                details += juceString(parameter.choices[index]);
            }
            details += "]";
        }
    }
}

[[nodiscard]] ComponentReplacementPresentationState presentationState(
    replacement::ComponentBundleReplacementControllerState state)
{
    using ControllerState =
        replacement::ComponentBundleReplacementControllerState;
    switch (state)
    {
    case ControllerState::Idle:
        return ComponentReplacementPresentationState::Hidden;
    case ControllerState::Inspecting:
        return ComponentReplacementPresentationState::Inspecting;
    case ControllerState::AwaitingOwnerApproval:
        return ComponentReplacementPresentationState::AwaitingOwnerApproval;
    case ControllerState::AwaitingConfirmation:
        return ComponentReplacementPresentationState::AwaitingConfirmation;
    case ControllerState::Probing:
        return ComponentReplacementPresentationState::Probing;
    case ControllerState::Activating:
        return ComponentReplacementPresentationState::Activating;
    case ControllerState::Staging:
        return ComponentReplacementPresentationState::Staging;
    case ControllerState::Succeeded:
        return ComponentReplacementPresentationState::Succeeded;
    case ControllerState::Failed:
        return ComponentReplacementPresentationState::Failed;
    case ControllerState::Cancelled:
        return ComponentReplacementPresentationState::Cancelled;
    }
    return ComponentReplacementPresentationState::Failed;
}

[[nodiscard]] juce::String replacementHeadline(
    ComponentReplacementPresentationState state)
{
    switch (state)
    {
    case ComponentReplacementPresentationState::Hidden:
        return {};
    case ComponentReplacementPresentationState::Inspecting:
        return "Inspecting exact candidate";
    case ComponentReplacementPresentationState::AwaitingOwnerApproval:
        return "Approve exact candidate";
    case ComponentReplacementPresentationState::AwaitingConfirmation:
        return "Review exact replacement";
    case ComponentReplacementPresentationState::Probing:
        return "Validating in disposable Probe";
    case ComponentReplacementPresentationState::Activating:
        return "Preparing isolated Runtime";
    case ComponentReplacementPresentationState::Staging:
        return "Staging replacement graph";
    case ComponentReplacementPresentationState::Succeeded:
        return "Replacement staged";
    case ComponentReplacementPresentationState::Failed:
        return "Replacement rejected";
    case ComponentReplacementPresentationState::Cancelled:
        return "Replacement cancelled";
    }
    return "Component replacement";
}

[[nodiscard]] ComponentReplacementPresentation replacementPresentation(
    const replacement::ComponentBundleReplacementControllerSnapshot& snapshot)
{
    ComponentReplacementPresentation presentation;
    presentation.state = presentationState(snapshot.state);
    presentation.headline = replacementHeadline(presentation.state);
    presentation.status = juceString(snapshot.message);
    presentation.canConfirm = presentation.state
            == ComponentReplacementPresentationState::AwaitingOwnerApproval
        || presentation.state
            == ComponentReplacementPresentationState::AwaitingConfirmation;
    if (presentation.state
        == ComponentReplacementPresentationState::AwaitingOwnerApproval)
    {
        presentation.confirmLabel = "Approve candidate";
    }
    else if (presentation.state
        == ComponentReplacementPresentationState::AwaitingConfirmation)
    {
        presentation.confirmLabel = snapshot.preview == nullptr
            ? juce::String{"Confirm replacement"}
            : juceString("Confirm ")
                + juceString(snapshot.preview->candidateVersion)
                + juceString(" · SHA-256 ")
                + digestText(snapshot.preview->candidateContentDigest);
    }
    presentation.canChoose = presentation.canConfirm
        || presentation.state
            == ComponentReplacementPresentationState::Failed
        || presentation.state
            == ComponentReplacementPresentationState::Cancelled;
    presentation.canCancel = presentation.visible();

    if (snapshot.preview == nullptr)
    {
        if (snapshot.candidate.available)
        {
            presentation.details = juce::String{"Candidate: "}
                + juceString(snapshot.candidate.bundleId)
                + " @ " + juceString(snapshot.candidate.semanticVersion)
                + "\nPublisher: " + juceString(snapshot.candidate.publisher)
                + "\nLicense: "
                + juceString(snapshot.candidate.licenseExpression)
                + "\nRequested permissions: 0x"
                + juce::String::toHexString(static_cast<juce::int64>(
                    snapshot.candidate.requestedPermissions))
                + "\nContent SHA-256: "
                + digestText(snapshot.candidate.contentDigest)
                + "\nSigner SHA-256: "
                + digestText(snapshot.candidate.signerIdentityDigest)
                + "\nDecision: Exact session-local owner approval is required."
                  "\n\nApproval is local to this Workbench session; approval does not stage or execute the candidate.";
            return presentation;
        }
        presentation.details =
            "The selected directory is inspected without scanning sibling locations.\n"
            "Project bytes and the active graph remain unchanged until exact confirmation and stage admission.";
        return presentation;
    }

    const auto& preview = *snapshot.preview;
    auto details = juce::String{"Preserved: "}
        + juceString(preview.preservedBundleId)
        + " @ " + juceString(preview.preservedVersion)
        + "\nCandidate: " + juceString(preview.candidateBundleId)
        + " @ " + juceString(preview.candidateVersion)
        + "\nNode type: " + juceString(preview.nodeTypeId)
        + "\nPublisher: " + juceString(preview.publisher)
        + "\nLicense: " + juceString(preview.licenseExpression)
        + "\nState schema: "
        + juce::String(static_cast<juce::int64>(
            preview.stateSchemaVersion))
        + "\nRequested permissions: 0x"
        + juce::String::toHexString(static_cast<juce::int64>(
            preview.requestedPermissions))
        + "\nContent SHA-256: "
        + digestText(preview.candidateContentDigest)
        + "\nSigner SHA-256: "
        + digestText(preview.candidateSignerIdentityDigest)
        + "\nDecision: " + juceString(preview.diagnostic);
    appendPorts(details, "Inputs", preview.inputs);
    appendPorts(details, "Outputs", preview.outputs);
    appendParameters(details, preview.parameters);
    if (preview.resourceTransitions.empty())
    {
        details += "\nResources: unchanged";
    }
    else
    {
        details += "\nResource transitions:";
        for (const auto& transition : preview.resourceTransitions)
        {
            details += "\n  "
                + juceString(transition.previous.resourceId)
                + juceString(" · ")
                + resourceKindText(transition.previous.kind)
                + " g" + juce::String(static_cast<juce::int64>(
                    transition.previous.generation))
                + " -> " + resourceKindText(transition.candidate.kind)
                + " g" + juce::String(static_cast<juce::int64>(
                    transition.candidate.generation));
            details += "\n    old SHA-256: "
                + juceString(transition.previous.contentHash)
                + "\n    new SHA-256: "
                + juceString(transition.candidate.contentHash);
            if (transition.requiresExplicitConfirmation)
                details += juceString(" · explicit confirmation");
        }
    }
    presentation.details = std::move(details);
    return presentation;
}
#endif
}

void WorkbenchComponent::beginComponentReplacement(
    std::optional<std::string> definitionId,
    std::string nodeId)
{
#if !defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    juce::ignoreUnused(definitionId, nodeId);
    setStatus(
        "Component replacement remains inside the local validation gate");
#else
    if (definitionId.has_value())
    {
        setStatus(
            "Component replacement is currently limited to preserved nodes in the root graph");
        return;
    }
    const auto* node = nodeForContext(std::nullopt, nodeId);
    if (node == nullptr || !node->preservedComponent.has_value())
    {
        setStatus("The selected preserved node is no longer available");
        return;
    }
    if (fileChooserActive_ || destructivePromptActive_
        || exportDialog_ != nullptr || exportRunning_ || packageRunning_)
    {
        setStatus(
            "Finish the active dialog or project operation before replacing a component");
        return;
    }
    if (componentReplacementState_ == nullptr)
    {
        try
        {
            componentReplacementState_.reset(
                new ComponentReplacementControllerState{});
        }
        catch (...)
        {
            setStatus(
                "Component replacement session storage is unavailable");
            return;
        }
    }
    auto& replacementState = *componentReplacementState_;
    if (replacementState.controller != nullptr)
    {
        const auto current = replacementState.controller->snapshot().state;
        const auto replaceable =
            current
                == replacement::ComponentBundleReplacementControllerState::
                    AwaitingOwnerApproval
            || current
                == replacement::ComponentBundleReplacementControllerState::
                    AwaitingConfirmation
            || current
                == replacement::ComponentBundleReplacementControllerState::
                    Failed
            || current
                == replacement::ComponentBundleReplacementControllerState::
                    Cancelled;
        if (!replaceable || replacementState.nodeId != nodeId)
        {
            setStatus(
                "Finish the active component replacement before starting another");
            return;
        }
        stopComponentReplacement();
    }

    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Choose One Component Bundle Directory",
        juce::File::getSpecialLocation(
            juce::File::userDocumentsDirectory),
        "*");
    fileChooserActive_ = true;
    inspectorPanel_->setComponentReplacementEnabled(false);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis, nodeId = std::move(nodeId)](
            const juce::FileChooser& chooser) mutable {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
            {
                safeThis->inspectorPanel_
                    ->setComponentReplacementEnabled(true);
                safeThis->setStatus("Component replacement selection cancelled");
                return;
            }
            const auto path = filesystemPath(selected.getFullPathName());
            safeThis->scheduleContinuation(
                [safeThis, nodeId = std::move(nodeId), path] {
                    if (safeThis == nullptr)
                        return;
                    const auto* currentNode = safeThis->nodeForContext(
                        std::nullopt,
                        nodeId);
                    if (currentNode == nullptr
                        || !currentNode->preservedComponent.has_value())
                    {
                        safeThis->setStatus(
                            "The selected preserved node changed before inspection");
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    replacement::ComponentBundleReplacementControllerInputs
                        inputs;
                    inputs.bridge = std::addressof(safeThis->projectBridge_);
                    auto& replacementState =
                        *safeThis->componentReplacementState_;
                    inputs.ownerAuthorization = std::addressof(
                        replacementState.ownerAuthorization);
                    inputs.document =
                        safeThis->projectBridge_.editor().document();
                    if (auto* device = safeThis->deviceManager_
                            .getCurrentAudioDevice();
                        device != nullptr
                        && device->getCurrentSampleRate() > 0.0
                        && device->getCurrentBufferSizeSamples() > 0)
                    {
                        inputs.renderSettings.sampleRate =
                            device->getCurrentSampleRate();
                        inputs.renderSettings.maxBlockFrames =
                            static_cast<std::size_t>(
                                device->getCurrentBufferSizeSamples());
                    }
                    else
                    {
                        inputs.renderSettings.sampleRate = 48'000.0;
                        inputs.renderSettings.maxBlockFrames = 256;
                    }
                    inputs.renderSettings.enableRealtimeAnalysisCapture = true;
                    if (const auto packageError =
                            safeThis->currentPackageStagingError();
                        packageError.has_value())
                    {
                        safeThis->setStatus(
                            "Replacement asset validation failed: "
                            + juceString(*packageError));
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    const auto beforeLoad = talas::audio::
                        captureProjectAssetWatchSnapshot(
                            inputs.document,
                            safeThis->registry_,
                            safeThis->projectPath());
                    auto loaded = talas::audio::
                        loadReferencedProjectResourceSnapshots(
                            inputs.document,
                            safeThis->registry_,
                            safeThis->projectPath());
                    if (!loaded.ok())
                    {
                        const auto detail = loaded.failure.has_value()
                            ? talas::audio::describeProjectAssetLoadFailure(
                                *loaded.failure)
                            : std::string{"Asset loading was cancelled"};
                        safeThis->setStatus(juceString(detail));
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    if (const auto packageError =
                            safeThis->currentPackageStagingError();
                        packageError.has_value())
                    {
                        safeThis->setStatus(
                            "Package changed while loading replacement assets: "
                            + juceString(*packageError));
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    if (const auto changedAsset =
                            safeThis->firstChangedLoadedResourceSnapshot(
                                loaded.snapshots);
                        changedAsset.has_value())
                    {
                        safeThis->setStatus(
                            "Asset changed after loading replacement snapshot: "
                            + juceString(*changedAsset));
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    const auto afterLoad = talas::audio::
                        captureProjectAssetWatchSnapshot(
                            inputs.document,
                            safeThis->registry_,
                            safeThis->projectPath());
                    if (const auto change = talas::audio::
                            findProjectAssetWatchChange(beforeLoad, afterLoad);
                        change.has_value())
                    {
                        safeThis->setStatus(
                            "Asset changed while preparing replacement: "
                            + juceString(change->assetId));
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(true);
                        return;
                    }
                    inputs.renderSettings.resourceSnapshots =
                        std::move(loaded.snapshots);
                    for (const auto& snapshot :
                         inputs.renderSettings.resourceSnapshots)
                    {
                        if (snapshot == nullptr || snapshot->generation == 0)
                            continue;
                        const auto accepted = safeThis->projectBridge_
                            .acceptedResourceIdentity(snapshot->id);
                        if (!accepted.has_value())
                            continue;
                        if (snapshot->kind != accepted->kind)
                        {
                            safeThis->setStatus(
                                "Resource kind transitions remain closed on the owner-selected Workbench replacement surface");
                            safeThis->inspectorPanel_
                                ->setComponentReplacementEnabled(true);
                            return;
                        }
                        const auto identityChanged =
                            snapshot->generation != accepted->generation
                            || snapshot->contentHash != accepted->contentHash;
                        if (!identityChanged)
                            continue;
                        if (inputs.resourceTransitions.size()
                            >= replacement::
                                maximumComponentBundleReplacementTransitions)
                        {
                            safeThis->setStatus(
                                "Replacement exceeds the closed eight-resource transition limit");
                            safeThis->inspectorPanel_
                                ->setComponentReplacementEnabled(true);
                            return;
                        }
                        inputs.resourceTransitions.push_back({
                            {
                                snapshot->id,
                                accepted->kind,
                                accepted->generation,
                                accepted->contentHash,
                            },
                            {
                                snapshot->id,
                                snapshot->kind,
                                snapshot->generation,
                                snapshot->contentHash,
                            },
                            false,
                        });
                    }
                    inputs.documentGeneration =
                        replacementState.documentGeneration->load(
                            std::memory_order_acquire);
                    inputs.liveDocumentGeneration =
                        replacementState.documentGeneration;
                    inputs.nodeId = nodeId;
                    const auto analysisSampleRate =
                        inputs.renderSettings.sampleRate;
                    auto prepared = replacement::
                        ComponentBundleReplacementController::prepare(
                            std::move(inputs));
                    if (!prepared.ok())
                    {
                        ComponentReplacementPresentation failed;
                        failed.state =
                            ComponentReplacementPresentationState::Failed;
                        failed.headline = "Replacement unavailable";
                        failed.status =
                            "The exact replacement controller could not be prepared";
                        failed.details =
                            "Project bytes and the active graph were not changed.";
                        failed.canChoose = true;
                        failed.canCancel = true;
                        safeThis->inspectorPanel_
                            ->setComponentReplacementPresentation(failed);
                        safeThis->inspectorPanel_
                            ->setComponentReplacementEnabled(false);
                        return;
                    }
                    replacementState.nodeId = nodeId;
                    replacementState.analysisSampleRate =
                        analysisSampleRate;
                    replacementState.verifiedAssetWatchBaseline =
                        std::move(afterLoad);
                    replacementState.productStageIntegrated = false;
                    replacementState.surfaceDismissed = false;
                    replacementState.controller =
                        std::move(prepared.controller);
                    if (replacementState.controller->inspect(path)
                        != replacement::
                            ComponentBundleReplacementControllerOperation::
                                Scheduled)
                    {
                        safeThis->stopComponentReplacement();
                        safeThis->setStatus(
                            "Component replacement inspection could not start");
                        return;
                    }
                    safeThis->finishComponentReplacementIfReady();
                    safeThis->setStatus(
                        "Inspecting one explicitly selected component bundle");
                });
        });
#endif
}

void WorkbenchComponent::confirmComponentReplacement(
    std::optional<std::string> definitionId,
    std::string_view nodeId)
{
#if !defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    juce::ignoreUnused(definitionId, nodeId);
#else
    if (definitionId.has_value()
        || componentReplacementState_ == nullptr
        || componentReplacementState_->controller == nullptr
        || componentReplacementState_->nodeId != nodeId)
    {
        setStatus("The replacement preview is stale");
        return;
    }
    auto& replacementState = *componentReplacementState_;
    const auto snapshot = replacementState.controller->snapshot();
    if (snapshot.state
        == replacement::ComponentBundleReplacementControllerState::
            AwaitingOwnerApproval)
    {
        if (replacementState.controller->approveCandidate()
            != replacement::
                ComponentBundleReplacementControllerOperation::Scheduled)
        {
            setStatus("The exact session-local owner approval was rejected");
            finishComponentReplacementIfReady();
            return;
        }
        finishComponentReplacementIfReady();
        setStatus(
            "Candidate approved for this session; review compatibility before replacement");
        return;
    }
    if (snapshot.state
            != replacement::ComponentBundleReplacementControllerState::
                AwaitingConfirmation
        || snapshot.previewGeneration == 0
        || replacementState.controller->confirm(snapshot.previewGeneration)
            != replacement::
                ComponentBundleReplacementControllerOperation::Scheduled)
    {
        setStatus(
            "The exact replacement confirmation was rejected as stale");
        finishComponentReplacementIfReady();
        return;
    }
    finishComponentReplacementIfReady();
    setStatus(
        "Replacement confirmed locally; Probe and Runtime validation started");
#endif
}

void WorkbenchComponent::cancelComponentReplacement(bool dismissOnly)
{
#if !defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    juce::ignoreUnused(dismissOnly);
#else
    if (componentReplacementState_ == nullptr
        || componentReplacementState_->controller == nullptr)
    {
        inspectorPanel_->setComponentReplacementPresentation({});
        inspectorPanel_->setComponentReplacementEnabled(true);
        return;
    }
    auto& replacementState = *componentReplacementState_;
    const auto state = replacementState.controller->snapshot().state;
    if (dismissOnly
        || state
            == replacement::ComponentBundleReplacementControllerState::
                Succeeded)
    {
        replacementState.surfaceDismissed = true;
        inspectorPanel_->setComponentReplacementPresentation({});
        inspectorPanel_->setComponentReplacementEnabled(false);
        setStatus(
            "Replacement remains active for this session; project bytes are unchanged");
        return;
    }
    static_cast<void>(replacementState.controller->cancel());
    replacementState.controller.reset();
    replacementState.nodeId.clear();
    replacementState.analysisSampleRate = 0.0;
    replacementState.verifiedAssetWatchBaseline = {};
    replacementState.productStageIntegrated = false;
    replacementState.surfaceDismissed = false;
    inspectorPanel_->setComponentReplacementPresentation({});
    inspectorPanel_->setComponentReplacementEnabled(true);
    setStatus("Component replacement cancelled; previous project state kept");
#endif
}

void WorkbenchComponent::finishComponentReplacementIfReady()
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    if (componentReplacementState_ == nullptr
        || componentReplacementState_->controller == nullptr)
    {
        inspectorPanel_->setComponentReplacementEnabled(
            !fileChooserActive_ && !destructivePromptActive_);
        return;
    }
    auto& replacementState = *componentReplacementState_;
    auto snapshot = replacementState.controller->snapshot();
    if (snapshot.state
        == replacement::ComponentBundleReplacementControllerState::Staging)
    {
        const auto currentAssets =
            talas::audio::captureProjectAssetWatchSnapshot(
                projectBridge_.editor().document(), registry_, projectPath());
        const auto changedAsset = talas::audio::findProjectAssetWatchChange(
            replacementState.verifiedAssetWatchBaseline, currentAssets);
        if (changedAsset.has_value())
        {
            static_cast<void>(replacementState.controller->cancel());
            setStatus(
                "Asset changed before replacement stage: "
                + juceString(changedAsset->assetId));
        }
        else
        {
            static_cast<void>(replacementState.controller->stage());
        }
        snapshot = replacementState.controller->snapshot();
    }
    inspectorPanel_->setComponentReplacementEnabled(false);
    if (snapshot.state
            == replacement::ComponentBundleReplacementControllerState::Succeeded
        && !replacementState.productStageIntegrated)
    {
        replacementState.productStageIntegrated = true;
        currentDocumentStaged_ = true;
        assetWatchBaseline_ = replacementState.verifiedAssetWatchBaseline;
        nextAssetWatchPollMilliseconds_ =
            juce::Time::getMillisecondCounterHiRes() + 1000.0;
        publishCompileDiagnostics({});
        analysisPanel_->setCapture(
            projectBridge_.latestAnalysisCapture(),
            replacementState.analysisSampleRate,
            projectBridge_.latestScoreSourceAnalysisCaptures());
        stagedVirtualControls_ =
            projectBridge_.editor().document().virtualControls;
        stagedMidiRoutesValid_ = true;
        rebuildMidiRuntimeRoutes();
        rebuildOscIngress();
        refreshPreparedGraphStatus();
        commandManager_.commandStatusChanged();
    }
    if (replacementState.surfaceDismissed
        || activeGraphContext_.has_value()
        || selectedNodeId_ != replacementState.nodeId)
    {
        inspectorPanel_->setComponentReplacementPresentation({});
        return;
    }
    const auto presentation = replacementPresentation(snapshot);
    inspectorPanel_->setComponentReplacementPresentation(presentation);
    if (snapshot.state
        == replacement::ComponentBundleReplacementControllerState::Succeeded)
    {
        const auto version = snapshot.preview != nullptr
            ? juceString(snapshot.preview->candidateVersion)
            : juce::String{"unknown version"};
        setStatus(
            "Staged exact component replacement "
            + version
            + "; canonical project bytes remain unchanged");
    }
#endif
}

void WorkbenchComponent::stopComponentReplacement() noexcept
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    if (componentReplacementState_ != nullptr)
    {
        if (componentReplacementState_->controller != nullptr)
        {
            static_cast<void>(
                componentReplacementState_->controller->cancel());
        }
        componentReplacementState_->controller.reset();
        componentReplacementState_->nodeId.clear();
        componentReplacementState_->analysisSampleRate = 0.0;
        componentReplacementState_->verifiedAssetWatchBaseline = {};
        componentReplacementState_->productStageIntegrated = false;
        componentReplacementState_->surfaceDismissed = false;
    }
    if (inspectorPanel_ != nullptr)
    {
        inspectorPanel_->setComponentReplacementPresentation({});
        inspectorPanel_->setComponentReplacementEnabled(true);
    }
#endif
}

void WorkbenchComponent::invalidateComponentReplacementForDocumentChange()
    noexcept
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    if (componentReplacementState_ != nullptr)
    {
        componentReplacementState_->documentGeneration->fetch_add(
            1,
            std::memory_order_acq_rel);
    }
    stopComponentReplacement();
#endif
}
}
