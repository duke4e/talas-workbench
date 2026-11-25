#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include "runtime/component_activation/ComponentActivationStageGate.h"

#include <atomic>
#include <new>
#include <string>

namespace talas::runtime::detail
{
struct ComponentProbeEvidencePayloadV1
{
    std::string bundleId;
    std::string semanticVersion;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t policyVersion = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::uint64_t artifactGeneration = 0;
    std::uint32_t protocolVersion = 0;
    ComponentProbeState state = ComponentProbeState::NotRun;
    bool hasCandidateFingerprint = false;
    bool hasValidationNonce = false;
};

struct ComponentProbeValidationSessionStateV1
{
    std::shared_ptr<ComponentActivationStageGateV1> stageGate;
    std::atomic<bool> active{true};
};
}

namespace talas::runtime
{
bool ComponentProbeLivenessV1::active() const noexcept
{
    return state_ && state_->active.load(std::memory_order_acquire);
}

ComponentProbeEvidenceView ComponentProbeEvidence::inspect() const noexcept
{
    ComponentProbeEvidenceView view;
    if (!payload_)
        return view;
    view.state = payload_->state;
    view.candidateFingerprint.bundleId = payload_->bundleId;
    view.candidateFingerprint.semanticVersion = payload_->semanticVersion;
    view.candidateFingerprint.contentDigest = payload_->contentDigest;
    view.candidateFingerprint.signerIdentityDigest =
        payload_->signerIdentityDigest;
    view.candidateFingerprint.requestedPermissions =
        payload_->requestedPermissions;
    view.candidateFingerprint.policyVersion = payload_->policyVersion;
    view.candidateFingerprint.hasContentDigest = true;
    view.candidateFingerprint.hasSignerIdentity = true;
    view.validationNonce = payload_->validationNonce;
    view.artifactGeneration = payload_->artifactGeneration;
    view.protocolVersion = payload_->protocolVersion;
    view.hasCandidateFingerprint = payload_->hasCandidateFingerprint;
    view.hasValidationNonce = payload_->hasValidationNonce;
    view.authored = true;
    const auto session = session_.lock();
    view.sessionActive = session
        && session->active.load(std::memory_order_acquire);
    return view;
}

ComponentProbeLivenessV1 ComponentProbeEvidence::acquireLiveness()
    const noexcept
{
    if (!payload_ || payload_->state != ComponentProbeState::Clean)
        return {};
    auto session = session_.lock();
    if (!session || !session->active.load(std::memory_order_acquire))
        return {};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!session->active.load(std::memory_order_acquire))
        return {};
    return ComponentProbeLivenessV1{
        session,
        session->stageGate};
}
}

namespace talas::runtime::component_probe
{
namespace
{
[[nodiscard]] bool knownTerminalState(ComponentProbeState state) noexcept
{
    switch (state)
    {
        case ComponentProbeState::Clean:
        case ComponentProbeState::Rejected:
        case ComponentProbeState::ControlledFatal:
        case ComponentProbeState::AbnormalTermination:
        case ComponentProbeState::TimedOut:
        case ComponentProbeState::ProtocolError:
            return true;
        case ComponentProbeState::NotRun:
            return false;
    }
    return false;
}

template <std::size_t Size>
[[nodiscard]] bool allZero(
    const std::array<std::uint8_t, Size>& value) noexcept
{
    for (const auto byte : value)
    {
        if (byte != 0)
            return false;
    }
    return true;
}

[[nodiscard]] bool structurallyValid(
    const ComponentProbeEvidenceView& view) noexcept
{
    if (view.authored || view.sessionActive || !knownTerminalState(view.state)
        || !view.hasCandidateFingerprint || !view.hasValidationNonce
        || view.protocolVersion != currentComponentProbeProtocolVersion
        || view.artifactGeneration == 0 || allZero(view.validationNonce))
    {
        return false;
    }
    const auto match = matchComponentOwnerApproval(
        view.candidateFingerprint,
        &view.candidateFingerprint);
    return match.ownerApproval == ComponentOwnerApprovalState::ExactMatch
        && match.diagnostic == ComponentOwnerApprovalMatchCode::ExactMatch;
}

[[nodiscard]] ComponentProbeEvidenceView viewFor(
    ComponentProbeState state,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    ComponentProbeEvidenceView view;
    view.state = state;
    view.candidateFingerprint.bundleId = std::string_view{
        request.bundleId.data(), request.bundleIdBytes};
    view.candidateFingerprint.semanticVersion = std::string_view{
        request.semanticVersion.data(), request.semanticVersionBytes};
    view.candidateFingerprint.contentDigest = request.contentDigest;
    view.candidateFingerprint.signerIdentityDigest =
        request.signerIdentityDigest;
    view.candidateFingerprint.requestedPermissions =
        request.requestedPermissions;
    view.candidateFingerprint.policyVersion = request.policyVersion;
    view.candidateFingerprint.hasContentDigest = true;
    view.candidateFingerprint.hasSignerIdentity = true;
    view.validationNonce = request.validationNonce;
    view.artifactGeneration = request.artifactGeneration;
    view.protocolVersion = currentComponentProbeProtocolVersion;
    view.hasCandidateFingerprint = true;
    view.hasValidationNonce = true;
    return view;
}

}

std::optional<ComponentBundleProbeEvidenceAuthoringV1>
ComponentBundleProbeEvidenceFactory::author(
    ComponentProbeEvidenceView view) noexcept
{
    if (!structurallyValid(view))
        return std::nullopt;
    try
    {
        auto payload = std::make_shared<detail::ComponentProbeEvidencePayloadV1>();
        payload->bundleId = view.candidateFingerprint.bundleId;
        payload->semanticVersion = view.candidateFingerprint.semanticVersion;
        payload->contentDigest = view.candidateFingerprint.contentDigest;
        payload->signerIdentityDigest =
            view.candidateFingerprint.signerIdentityDigest;
        payload->requestedPermissions =
            view.candidateFingerprint.requestedPermissions;
        payload->policyVersion = view.candidateFingerprint.policyVersion;
        payload->validationNonce = view.validationNonce;
        payload->artifactGeneration = view.artifactGeneration;
        payload->protocolVersion = view.protocolVersion;
        payload->state = view.state;
        payload->hasCandidateFingerprint = view.hasCandidateFingerprint;
        payload->hasValidationNonce = view.hasValidationNonce;
        auto session = std::make_shared<
            detail::ComponentProbeValidationSessionStateV1>();
        session->stageGate = std::make_shared<
            detail::ComponentActivationStageGateV1>();
        ComponentBundleProbeEvidenceAuthoringV1 authored{
            ComponentBundleProbeValidationSessionV1{session},
            ComponentProbeEvidence{std::move(payload), session},
        };
        return std::optional<ComponentBundleProbeEvidenceAuthoringV1>{
            std::move(authored)};
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
}

ComponentBundleProbeValidationSessionV1::
    ComponentBundleProbeValidationSessionV1(
        std::shared_ptr<detail::ComponentProbeValidationSessionStateV1> state)
        noexcept
    : state_(std::move(state))
{
}

ComponentBundleProbeValidationSessionV1::
    ~ComponentBundleProbeValidationSessionV1()
{
    revoke();
}

ComponentBundleProbeValidationSessionV1::
    ComponentBundleProbeValidationSessionV1(
        ComponentBundleProbeValidationSessionV1&&) noexcept = default;

ComponentBundleProbeValidationSessionV1&
ComponentBundleProbeValidationSessionV1::operator=(
    ComponentBundleProbeValidationSessionV1&& other) noexcept
{
    if (this != &other)
    {
        revoke();
        state_ = std::move(other.state_);
    }
    return *this;
}

void ComponentBundleProbeValidationSessionV1::revoke() noexcept
{
    if (state_)
    {
        const std::unique_lock stageLock(state_->stageGate->mutex);
        state_->active.store(false, std::memory_order_release);
    }
}

std::optional<ComponentBundleProbeEvidenceAuthoringV1>
ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
    const ComponentBundleProbeResultV1& result,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (validateComponentBundleProbeResultV1(result, request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return std::nullopt;
    }
    return author(viewFor(
        static_cast<ComponentProbeState>(result.state),
        request));
}

std::optional<ComponentBundleProbeEvidenceAuthoringV1>
ComponentBundleProbeEvidenceFactory::makeParentClassifiedFailure(
    ComponentProbeState state,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (validateComponentBundleProbeRequestV1(request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid
        || (state != ComponentProbeState::AbnormalTermination
            && state != ComponentProbeState::TimedOut
            && state != ComponentProbeState::ProtocolError))
    {
        return std::nullopt;
    }
    return author(viewFor(state, request));
}
}
