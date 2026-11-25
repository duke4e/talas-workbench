#pragma once

#include "talas/core/ResourceSnapshot.h"
#include "talas/runtime/ComponentActivationProtocol.h"
#include "runtime/component_activation/ComponentActivationAudioSourceFreezer.h"
#include "runtime/component_bundle/ComponentBundleInspection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <shared_mutex>
#include <string_view>

namespace talas::runtime::detail
{
struct ComponentActivationRuntimeLiveStateV1;
struct ComponentActivationAuthorityLiveStateV1;
}

namespace talas::runtime::component_activation
{
class ComponentActivationMacOSHost;
class ComponentActivationRuntimeEvidenceSourceV1;
struct ComponentActivationAuthorizationTestPeer;

class ComponentActivationRuntimeSessionV1
{
public:
    ComponentActivationRuntimeSessionV1() noexcept = default;
    ~ComponentActivationRuntimeSessionV1();

    ComponentActivationRuntimeSessionV1(
        ComponentActivationRuntimeSessionV1&&) noexcept;
    ComponentActivationRuntimeSessionV1& operator=(
        ComponentActivationRuntimeSessionV1&&) noexcept;

    ComponentActivationRuntimeSessionV1(
        const ComponentActivationRuntimeSessionV1&) = delete;
    ComponentActivationRuntimeSessionV1& operator=(
        const ComponentActivationRuntimeSessionV1&) = delete;

    void revoke() noexcept;

private:
    explicit ComponentActivationRuntimeSessionV1(
        std::shared_ptr<detail::ComponentActivationRuntimeLiveStateV1> state)
        noexcept;

    std::shared_ptr<detail::ComponentActivationRuntimeLiveStateV1> state_;

    friend class ComponentActivationAuthorizationFactory;
};

class ComponentActivationRuntimeEvidenceV1
{
public:
    ComponentActivationRuntimeEvidenceV1() noexcept = default;

    [[nodiscard]] bool active() const noexcept;

private:
    explicit ComponentActivationRuntimeEvidenceV1(
        std::weak_ptr<const detail::ComponentActivationRuntimeLiveStateV1>
            state) noexcept;

    std::weak_ptr<const detail::ComponentActivationRuntimeLiveStateV1> state_;

    friend class ComponentActivationAuthorizationFactory;
};

struct ComponentActivationRuntimeEvidenceAuthoringV1
{
    ComponentActivationRuntimeSessionV1 session;
    ComponentActivationRuntimeEvidenceV1 evidence;
};

struct ComponentActivationResourceSetInputV1
{
    talas::core::ResourceSnapshotPtr snapshot;
    const ComponentActivationFrozenAudioSourceV1* audioSource = nullptr;
};

struct ComponentActivationAuthorizationInputsV1
{
    const component_bundle::ComponentBundleSnapshot* snapshot = nullptr;
    ComponentSignatureEvidence signature;
    ComponentOwnerAuthorizationEvidenceV1 ownerAuthorization;
    ComponentProbeEvidence probe;
    ComponentActivationRuntimeEvidenceV1 runtime;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::string_view descriptorId;
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint16_t inputChannelCount = 0;
    std::uint16_t outputChannelCount = 0;
    std::uint16_t opaqueStateByteCount = 0;
    std::span<const ComponentActivationStaticParameterV1> staticParameters;
    talas::core::ResourceSnapshotPtr resourceSnapshot;
    const ComponentActivationFrozenAudioSourceV1* audioSource = nullptr;
    std::span<const ComponentActivationResourceSetInputV1>
        resourceSetInputs;
};

enum class ComponentActivationAuthorizationCodeV1 : std::uint8_t
{
    Ready,
    SnapshotInvalid,
    SignatureInvalid,
    OwnerNotApproved,
    ProbeInvalid,
    RuntimeInvalid,
    DescriptorInvalid,
    EntryInvalid,
    ProfileUnsupported,
    SessionBindingInvalid,
    ResourceInvalid,
    AudioSourceInvalid,
    AllocationFailed,
};

class ComponentActivationStageLeaseV1
{
public:
    ComponentActivationStageLeaseV1() noexcept = default;
    ~ComponentActivationStageLeaseV1();

    ComponentActivationStageLeaseV1(
        ComponentActivationStageLeaseV1&&) noexcept;
    ComponentActivationStageLeaseV1& operator=(
        ComponentActivationStageLeaseV1&&) noexcept;

    ComponentActivationStageLeaseV1(
        const ComponentActivationStageLeaseV1&) = delete;
    ComponentActivationStageLeaseV1& operator=(
        const ComponentActivationStageLeaseV1&) = delete;

    [[nodiscard]] bool active() const noexcept;

private:
    explicit ComponentActivationStageLeaseV1(
        std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1>
            state);

    // Admission must allocate no caller storage after a remote commit. Keep
    // authority alive until every lock is released, including during moves.
    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state_;
    std::shared_lock<std::shared_mutex> authorityLock_;
    std::shared_lock<std::shared_mutex> ownerLock_;
    std::shared_lock<std::shared_mutex> probeLock_;
    std::shared_lock<std::shared_mutex> runtimeLock_;

    friend class ComponentActivationLeaseV1;
};

class ComponentActivationLeaseV1
{
public:
    ComponentActivationLeaseV1() noexcept = default;
    ~ComponentActivationLeaseV1();

    ComponentActivationLeaseV1(ComponentActivationLeaseV1&&) noexcept;
    ComponentActivationLeaseV1& operator=(
        ComponentActivationLeaseV1&&) noexcept;

    ComponentActivationLeaseV1(const ComponentActivationLeaseV1&) = delete;
    ComponentActivationLeaseV1& operator=(
        const ComponentActivationLeaseV1&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const ComponentActivationAuthorizationV1* record()
        const noexcept;
    [[nodiscard]] std::optional<ComponentActivationStageLeaseV1>
    tryAcquireStageLease() noexcept;
    void revoke() noexcept;

private:
    [[nodiscard]] const ComponentActivationImmutableResourceGrantV1*
    resourceGrant() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> resourceBytes()
        const noexcept;
    [[nodiscard]] const ComponentActivationAudioStreamGrantV1*
    audioStreamGrant() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> audioSourceBytes()
        const noexcept;
    [[nodiscard]] const ComponentActivationResourceSetGrantV1*
    resourceSetGrant() const noexcept;
    [[nodiscard]] std::span<const ComponentActivationResourceSetMemberV1>
    resourceSetMembers() const noexcept;
    [[nodiscard]] const ComponentActivationImmutableResourceGrantV1*
    resourceSetImmutableGrant(std::size_t index) const noexcept;
    [[nodiscard]] std::span<const std::uint8_t>
    resourceSetImmutableBytes(std::size_t index) const noexcept;
    [[nodiscard]] const ComponentActivationAudioStreamGrantV1*
    resourceSetAudioGrant(std::size_t index) const noexcept;
    [[nodiscard]] std::span<const std::uint8_t>
    resourceSetAudioBytes(std::size_t index) const noexcept;

    explicit ComponentActivationLeaseV1(
        std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1>
            state) noexcept;

    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state_;

    friend class ComponentActivationAuthorityV1;
    friend class ComponentActivationMacOSHost;
    friend struct ComponentActivationAuthorizationTestPeer;
};

class ComponentActivationAuthorityV1
{
public:
    ComponentActivationAuthorityV1() noexcept = default;
    ~ComponentActivationAuthorityV1();

    ComponentActivationAuthorityV1(
        ComponentActivationAuthorityV1&&) noexcept;
    ComponentActivationAuthorityV1& operator=(
        ComponentActivationAuthorityV1&&) noexcept;

    ComponentActivationAuthorityV1(
        const ComponentActivationAuthorityV1&) = delete;
    ComponentActivationAuthorityV1& operator=(
        const ComponentActivationAuthorityV1&) = delete;

    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] std::optional<ComponentActivationLeaseV1> consume() noexcept;
    void revoke() noexcept;

private:
    explicit ComponentActivationAuthorityV1(
        std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1>
            state) noexcept;

    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state_;

    friend class ComponentActivationAuthorizationFactory;
};

struct ComponentActivationAuthorizationResultV1
{
    std::optional<ComponentActivationAuthorityV1> authority;
    ComponentActivationAuthorizationCodeV1 code =
        ComponentActivationAuthorizationCodeV1::SnapshotInvalid;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ComponentActivationAuthorizationCodeV1::Ready
            && authority.has_value() && authority->pending();
    }
};

class ComponentActivationAuthorizationFactory
{
public:
    [[nodiscard]] static ComponentActivationAuthorizationResultV1 make(
        const ComponentActivationAuthorizationInputsV1& inputs) noexcept;

private:
    [[nodiscard]] static std::optional<
        ComponentActivationRuntimeEvidenceAuthoringV1>
    authorRuntimeEvidence(
        const ComponentActivationSealedSessionV1& session) noexcept;

    friend class ComponentActivationRuntimeEvidenceSourceV1;
    friend struct ComponentActivationAuthorizationTestPeer;
};

class ComponentActivationRuntimeEvidenceSourceV1
{
private:
    [[nodiscard]] static std::optional<
        ComponentActivationRuntimeEvidenceAuthoringV1>
    author(const ComponentActivationSealedSessionV1& session) noexcept
    {
        return ComponentActivationAuthorizationFactory::authorRuntimeEvidence(
            session);
    }

    friend class ComponentActivationMacOSHost;
};
}
