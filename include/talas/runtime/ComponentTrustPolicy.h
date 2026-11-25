#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#if defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#endif

namespace talas::runtime
{
class ComponentIsolationMacOSHost;

namespace component_bundle
{
class ComponentOwnerAuthorizationStore;
class ComponentBundleSignatureEvidenceFactory;
}

namespace component_probe
{
class ComponentBundleProbeEvidenceFactory;
}

namespace component_activation
{
class ComponentActivationStageLeaseV1;
}

namespace detail
{
struct ComponentIsolationLiveEvidenceStateV1;
struct ComponentOwnerAuthorizationLiveEvidenceStateV1;
struct ComponentOwnerActivationUseStateV1;
struct ComponentProbeEvidencePayloadV1;
struct ComponentProbeValidationSessionStateV1;
struct ComponentActivationStageGateV1;
}

enum class ComponentSignatureState : std::uint8_t
{
    Missing,
    Verified,
    Invalid,
    Unsupported,
    Expired,
    Revoked,
};

enum class ComponentOwnerApprovalState : std::uint8_t
{
    Missing,
    ExactMatch,
    Stale,
    Denied,
    Invalid,
};

enum class ComponentProbeState : std::uint8_t
{
    NotRun,
    Clean,
    Rejected,
    ControlledFatal,
    AbnormalTermination,
    TimedOut,
    ProtocolError,
};

enum class ComponentIsolationState : std::uint8_t
{
    Unavailable,
    Ready,
};

enum class ComponentIsolationSafeguard : std::uint32_t
{
    BoundedVersionedIpc = std::uint32_t{1} << 0,
    HostOwnedSharedBuffers = std::uint32_t{1} << 1,
    DenyAmbientAuthority = std::uint32_t{1} << 2,
    WatchdogTimeout = std::uint32_t{1} << 3,
    CrashQuarantine = std::uint32_t{1} << 4,
    OffCallbackTeardown = std::uint32_t{1} << 5,
    CallbackSafeControlPlane = std::uint32_t{1} << 6,
    GenerationScopedActivation = std::uint32_t{1} << 7,
};

using ComponentIsolationSafeguards = std::uint32_t;

[[nodiscard]] constexpr ComponentIsolationSafeguards
componentIsolationSafeguard(ComponentIsolationSafeguard safeguard) noexcept
{
    return static_cast<ComponentIsolationSafeguards>(safeguard);
}

inline constexpr ComponentIsolationSafeguards
    allComponentIsolationSafeguardsV1 =
        componentIsolationSafeguard(
            ComponentIsolationSafeguard::BoundedVersionedIpc)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::HostOwnedSharedBuffers)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::DenyAmbientAuthority)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::WatchdogTimeout)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::CrashQuarantine)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::OffCallbackTeardown)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::CallbackSafeControlPlane)
        | componentIsolationSafeguard(
            ComponentIsolationSafeguard::GenerationScopedActivation);

inline constexpr std::uint32_t currentComponentIsolationProfileVersion = 1;

#if defined(__APPLE__) && defined(__MACH__) && TARGET_OS_OSX
inline constexpr bool componentIsolationBackendAvailableV1 = true;
#else
inline constexpr bool componentIsolationBackendAvailableV1 = false;
#endif

enum class ComponentExecutionPermission : std::uint32_t
{
    RealtimeAudio = std::uint32_t{1} << 0,
    ImmutableResourceRead = std::uint32_t{1} << 1,
    StateMigration = std::uint32_t{1} << 2,
    DeclarativeUi = std::uint32_t{1} << 3,
    BoundedAudioStreamRead = std::uint32_t{1} << 4,
};

using ComponentExecutionPermissions = std::uint32_t;

enum class ComponentExecutionRole : std::uint32_t
{
    RealtimeProcessor = std::uint32_t{1} << 0,
    ImmutableResourceConsumer = std::uint32_t{1} << 1,
    StateMigrator = std::uint32_t{1} << 2,
    DeclarativeUiProvider = std::uint32_t{1} << 3,
    BoundedAudioStreamConsumer = std::uint32_t{1} << 4,
};

using ComponentExecutionRoles = std::uint32_t;

[[nodiscard]] constexpr ComponentExecutionPermissions componentPermission(
    ComponentExecutionPermission permission) noexcept
{
    return static_cast<ComponentExecutionPermissions>(permission);
}

inline constexpr ComponentExecutionPermissions
    allComponentExecutionPermissionsV1 =
        componentPermission(ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead)
        | componentPermission(ComponentExecutionPermission::StateMigration)
        | componentPermission(ComponentExecutionPermission::DeclarativeUi)
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);

[[nodiscard]] constexpr ComponentExecutionRoles componentRole(
    ComponentExecutionRole role) noexcept
{
    return static_cast<ComponentExecutionRoles>(role);
}

inline constexpr ComponentExecutionRoles allComponentExecutionRolesV1 =
    componentRole(ComponentExecutionRole::RealtimeProcessor)
    | componentRole(ComponentExecutionRole::ImmutableResourceConsumer)
    | componentRole(ComponentExecutionRole::StateMigrator)
    | componentRole(ComponentExecutionRole::DeclarativeUiProvider)
    | componentRole(ComponentExecutionRole::BoundedAudioStreamConsumer);

[[nodiscard]] constexpr ComponentExecutionPermissions
componentPermissionsForRoles(ComponentExecutionRoles roles) noexcept
{
    ComponentExecutionPermissions permissions = 0;
    if ((roles & componentRole(ComponentExecutionRole::RealtimeProcessor)) != 0)
    {
        permissions |=
            componentPermission(ComponentExecutionPermission::RealtimeAudio);
    }
    if ((roles
            & componentRole(
                ComponentExecutionRole::ImmutableResourceConsumer))
        != 0)
    {
        permissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    }
    if ((roles & componentRole(ComponentExecutionRole::StateMigrator)) != 0)
    {
        permissions |=
            componentPermission(ComponentExecutionPermission::StateMigration);
    }
    if ((roles
            & componentRole(ComponentExecutionRole::DeclarativeUiProvider))
        != 0)
    {
        permissions |=
            componentPermission(ComponentExecutionPermission::DeclarativeUi);
    }
    if ((roles
            & componentRole(
                ComponentExecutionRole::BoundedAudioStreamConsumer))
        != 0)
    {
        permissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    }
    return permissions;
}

inline constexpr std::size_t componentIsolationEvidenceNonceBytesV1 = 16;

struct ComponentIsolationEvidenceViewV1
{
    ComponentIsolationState state = ComponentIsolationState::Unavailable;
    std::uint32_t profileVersion = 0;
    ComponentIsolationSafeguards safeguards = 0;
    ComponentExecutionPermissions supportedPermissions = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentIsolationEvidenceNonceBytesV1>
        validationNonce{};
    bool hasValidationNonce = false;

    [[nodiscard]] constexpr bool operator==(
        const ComponentIsolationEvidenceViewV1&) const noexcept = default;
};

class ComponentIsolationEvidenceV1
{
public:
    ComponentIsolationEvidenceV1() noexcept = default;

    [[nodiscard]] ComponentIsolationEvidenceViewV1 inspect() const noexcept;

    [[nodiscard]] bool operator==(
        const ComponentIsolationEvidenceV1& other) const noexcept
    {
        return !liveState_.owner_before(other.liveState_)
            && !other.liveState_.owner_before(liveState_);
    }

private:
    explicit ComponentIsolationEvidenceV1(
        std::weak_ptr<
            const detail::ComponentIsolationLiveEvidenceStateV1> liveState)
        noexcept
        : liveState_(std::move(liveState))
    {
    }

    std::weak_ptr<const detail::ComponentIsolationLiveEvidenceStateV1>
        liveState_;

    friend class ComponentIsolationMacOSHost;
};

enum class ComponentTrustDisposition : std::uint8_t
{
    Reject,
    PreserveOnly,
    EligibleForProbe,
    EligibleForIsolatedActivation,
};

enum class ComponentTrustDiagnosticCode : std::uint8_t
{
    Ready,
    UnknownEvidence,
    SignatureMissing,
    SignatureInvalid,
    SignatureUnsupported,
    SignatureExpired,
    SignatureRevoked,
    SignatureEvidenceMismatch,
    SignatureProfileUnsupported,
    ApprovalMissing,
    ApprovalStale,
    ApprovalDenied,
    ApprovalInvalid,
    RolesEmpty,
    RolesUnknown,
    PermissionsEmpty,
    PermissionsUnknown,
    PermissionsRoleMismatch,
    IsolationPermissionsUnknown,
    IsolationPermissionsUnsupported,
    IsolationEvidenceContradictory,
    IsolationProfileUnsupported,
    IsolationSafeguardsUnknown,
    IsolationSafeguardsIncomplete,
    IsolationBackendUnavailable,
    ProbeRequired,
    ProbeRejected,
    ProbeControlledFatal,
    ProbeCrashed,
    ProbeTimedOut,
    ProbeProtocolError,
    ProbeEvidenceContradictory,
    ProbeEvidenceMismatch,
    ProbeProfileUnsupported,
    ProbeStale,
    IsolationUnavailable,
};

inline constexpr std::uint32_t currentComponentTrustPolicyVersion = 1;
inline constexpr std::size_t maximumComponentBundleIdBytes = 128;
inline constexpr std::size_t maximumComponentSemanticVersionBytes = 64;
inline constexpr std::size_t componentTrustDigestBytes = 32;

struct ComponentApprovalFingerprint
{
    std::string_view bundleId;
    std::string_view semanticVersion;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t policyVersion = 0;
    bool hasContentDigest = false;
    bool hasSignerIdentity = false;

    [[nodiscard]] constexpr bool operator==(
        const ComponentApprovalFingerprint&) const noexcept = default;
};

enum class ComponentOwnerApprovalMatchCode : std::uint8_t
{
    ExactMatch,
    MissingApproval,
    InvalidCandidate,
    InvalidApproval,
    BundleIdMismatch,
    VersionMismatch,
    ContentDigestMismatch,
    SignerMismatch,
    PermissionsMismatch,
    PolicyVersionMismatch,
};

struct ComponentOwnerApprovalMatch
{
    ComponentOwnerApprovalState ownerApproval =
        ComponentOwnerApprovalState::Invalid;
    ComponentOwnerApprovalMatchCode diagnostic =
        ComponentOwnerApprovalMatchCode::InvalidCandidate;

    [[nodiscard]] constexpr bool operator==(
        const ComponentOwnerApprovalMatch&) const noexcept = default;
};

class ComponentOwnerAuthorizationLeaseV1
{
public:
    ComponentOwnerAuthorizationLeaseV1() noexcept = default;
    ~ComponentOwnerAuthorizationLeaseV1() = default;

    ComponentOwnerAuthorizationLeaseV1(
        ComponentOwnerAuthorizationLeaseV1&&) noexcept = default;
    ComponentOwnerAuthorizationLeaseV1& operator=(
        ComponentOwnerAuthorizationLeaseV1&&) noexcept = default;

    ComponentOwnerAuthorizationLeaseV1(
        const ComponentOwnerAuthorizationLeaseV1&) = delete;
    ComponentOwnerAuthorizationLeaseV1& operator=(
        const ComponentOwnerAuthorizationLeaseV1&) = delete;

    [[nodiscard]] bool approved() const noexcept
    {
        return approved_ && lock_.owns_lock();
    }

private:
    ComponentOwnerAuthorizationLeaseV1(
        std::shared_ptr<
            const detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>
            liveState,
        std::unique_lock<std::mutex> lock,
        bool approved) noexcept
        : liveState_(std::move(liveState)),
          lock_(std::move(lock)),
          approved_(approved)
    {
    }

    std::shared_ptr<
        const detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>
        liveState_;
    std::unique_lock<std::mutex> lock_;
    bool approved_ = false;

    friend class ComponentOwnerAuthorizationEvidenceV1;
};

class ComponentOwnerActivationUseV1
{
public:
    ComponentOwnerActivationUseV1() noexcept = default;
    ~ComponentOwnerActivationUseV1() = default;

    ComponentOwnerActivationUseV1(
        ComponentOwnerActivationUseV1&&) noexcept = default;
    ComponentOwnerActivationUseV1& operator=(
        ComponentOwnerActivationUseV1&&) noexcept = default;

    ComponentOwnerActivationUseV1(
        const ComponentOwnerActivationUseV1&) = delete;
    ComponentOwnerActivationUseV1& operator=(
        const ComponentOwnerActivationUseV1&) = delete;

    [[nodiscard]] bool active() const noexcept;

private:
    explicit ComponentOwnerActivationUseV1(
        std::shared_ptr<const detail::ComponentOwnerActivationUseStateV1>
            state,
        std::shared_ptr<detail::ComponentActivationStageGateV1> stageGate)
        noexcept
        : state_(std::move(state))
        , stageGate_(std::move(stageGate))
    {
    }

    std::shared_ptr<const detail::ComponentOwnerActivationUseStateV1> state_;
    std::shared_ptr<detail::ComponentActivationStageGateV1> stageGate_;

    friend class ComponentOwnerAuthorizationEvidenceV1;
    friend class component_activation::ComponentActivationStageLeaseV1;
};

class ComponentOwnerAuthorizationEvidenceV1
{
public:
    ComponentOwnerAuthorizationEvidenceV1() noexcept = default;

    [[nodiscard]] ComponentOwnerApprovalMatch inspect(
        const ComponentApprovalFingerprint& candidate) const noexcept;

    [[nodiscard]] ComponentOwnerAuthorizationLeaseV1
    acquireExactApprovalLease(
        const ComponentApprovalFingerprint& candidate) const noexcept;

    [[nodiscard]] ComponentOwnerActivationUseV1 acquireExactActivationUse(
        const ComponentApprovalFingerprint& candidate) const noexcept;

    [[nodiscard]] bool operator==(
        const ComponentOwnerAuthorizationEvidenceV1& other) const noexcept
    {
        return !liveState_.owner_before(other.liveState_)
            && !other.liveState_.owner_before(liveState_);
    }

private:
    explicit ComponentOwnerAuthorizationEvidenceV1(
        std::weak_ptr<
            const detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>
            liveState) noexcept
        : liveState_(std::move(liveState))
    {
    }

    std::weak_ptr<
        const detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>
        liveState_;

    friend class component_bundle::ComponentOwnerAuthorizationStore;
};

[[nodiscard]] ComponentOwnerApprovalMatch matchComponentOwnerApproval(
    const ComponentApprovalFingerprint& candidate,
    const ComponentApprovalFingerprint* approved) noexcept;

inline constexpr std::uint32_t currentComponentSignatureProfileVersion = 1;

struct ComponentSignatureEvidenceView
{
    ComponentSignatureState state = ComponentSignatureState::Missing;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    std::uint32_t profileVersion = 0;
    bool hasContentDigest = false;
    bool hasSignerIdentity = false;

    [[nodiscard]] constexpr bool operator==(
        const ComponentSignatureEvidenceView&) const noexcept = default;
};

class ComponentSignatureEvidence
{
public:
    ComponentSignatureEvidence() noexcept = default;

    [[nodiscard]] constexpr ComponentSignatureEvidenceView inspect()
        const noexcept
    {
        return view_;
    }

    [[nodiscard]] constexpr bool operator==(
        const ComponentSignatureEvidence&) const noexcept = default;

private:
    explicit constexpr ComponentSignatureEvidence(
        ComponentSignatureEvidenceView view) noexcept
        : view_(std::move(view))
    {
    }

    ComponentSignatureEvidenceView view_;

    friend class component_bundle::ComponentBundleSignatureEvidenceFactory;
};

inline constexpr std::uint32_t currentComponentProbeProtocolVersion = 1;
inline constexpr std::size_t componentProbeNonceBytes = 16;

struct ComponentProbeEvidenceView
{
    ComponentProbeState state = ComponentProbeState::NotRun;
    ComponentApprovalFingerprint candidateFingerprint;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::uint64_t artifactGeneration = 0;
    std::uint32_t protocolVersion = 0;
    bool hasCandidateFingerprint = false;
    bool hasValidationNonce = false;
    bool authored = false;
    bool sessionActive = false;

    [[nodiscard]] constexpr bool operator==(
        const ComponentProbeEvidenceView&) const noexcept = default;
};

class ComponentProbeLivenessV1
{
public:
    ComponentProbeLivenessV1() noexcept = default;
    ~ComponentProbeLivenessV1() = default;

    ComponentProbeLivenessV1(ComponentProbeLivenessV1&&) noexcept = default;
    ComponentProbeLivenessV1& operator=(
        ComponentProbeLivenessV1&&) noexcept = default;

    ComponentProbeLivenessV1(const ComponentProbeLivenessV1&) = delete;
    ComponentProbeLivenessV1& operator=(
        const ComponentProbeLivenessV1&) = delete;

    [[nodiscard]] bool active() const noexcept;

private:
    explicit ComponentProbeLivenessV1(
        std::shared_ptr<
            const detail::ComponentProbeValidationSessionStateV1> state,
        std::shared_ptr<detail::ComponentActivationStageGateV1> stageGate)
        noexcept
        : state_(std::move(state))
        , stageGate_(std::move(stageGate))
    {
    }

    std::shared_ptr<const detail::ComponentProbeValidationSessionStateV1>
        state_;
    std::shared_ptr<detail::ComponentActivationStageGateV1> stageGate_;

    friend class ComponentProbeEvidence;
    friend class component_activation::ComponentActivationStageLeaseV1;
};

class ComponentProbeEvidence
{
public:
    ComponentProbeEvidence() noexcept = default;

    [[nodiscard]] ComponentProbeEvidenceView inspect() const noexcept;

    [[nodiscard]] ComponentProbeLivenessV1 acquireLiveness() const noexcept;

    [[nodiscard]] bool operator==(
        const ComponentProbeEvidence& other) const noexcept
    {
        return payload_ == other.payload_
            && !session_.owner_before(other.session_)
            && !other.session_.owner_before(session_);
    }

private:
    ComponentProbeEvidence(
        std::shared_ptr<const detail::ComponentProbeEvidencePayloadV1> payload,
        std::weak_ptr<const detail::ComponentProbeValidationSessionStateV1>
            session) noexcept
        : payload_(std::move(payload)),
          session_(std::move(session))
    {
    }

    std::shared_ptr<const detail::ComponentProbeEvidencePayloadV1> payload_;
    std::weak_ptr<const detail::ComponentProbeValidationSessionStateV1>
        session_;

    friend class component_probe::ComponentBundleProbeEvidenceFactory;
};

struct ComponentTrustEvidence
{
    ComponentApprovalFingerprint candidate;
    ComponentOwnerAuthorizationEvidenceV1 ownerAuthorizationEvidence;
    ComponentSignatureEvidence signature;
    ComponentProbeEvidence probe;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::uint64_t artifactGeneration = 0;
    ComponentIsolationEvidenceV1 isolationEvidence;
    ComponentExecutionRoles declaredRoles = 0;
    bool hasValidationNonce = false;

    [[nodiscard]] bool operator==(
        const ComponentTrustEvidence&) const noexcept = default;
};

struct ComponentTrustDecision
{
    ComponentTrustDisposition disposition =
        ComponentTrustDisposition::Reject;
    ComponentTrustDiagnosticCode diagnostic =
        ComponentTrustDiagnosticCode::UnknownEvidence;

    [[nodiscard]] constexpr bool operator==(
        const ComponentTrustDecision&) const noexcept = default;
};

inline constexpr std::size_t maximumComponentTrustProjectionNameBytes = 48;
inline constexpr std::size_t maximumComponentTrustProjectionSummaryBytes = 128;

struct ComponentTrustProjection
{
    std::string_view disposition;
    std::string_view diagnostic;
    std::string_view summary;
    bool canExecute = false;
    bool canInstall = false;

    [[nodiscard]] constexpr bool operator==(
        const ComponentTrustProjection&) const noexcept = default;
};

[[nodiscard]] ComponentTrustProjection projectComponentTrust(
    ComponentTrustDecision decision) noexcept;

[[nodiscard]] ComponentTrustDecision evaluateComponentTrust(
    const ComponentTrustEvidence& evidence) noexcept;
}
