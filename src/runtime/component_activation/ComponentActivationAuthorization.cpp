#include "runtime/component_activation/ComponentActivationAuthorization.h"

#include "runtime/component_activation/ComponentActivationManifestProfile.h"
#include "runtime/component_activation/ComponentActivationStageGate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <shared_mutex>
#include <utility>

namespace talas::runtime::detail
{
struct ComponentActivationRuntimeLiveStateV1
{
    component_activation::ComponentActivationSealedSessionV1 session;
    std::shared_ptr<ComponentActivationStageGateV1> stageGate;
    std::atomic<bool> active{true};
};

enum class ComponentActivationAuthorityStatusV1 : std::uint8_t
{
    Pending,
    Consumed,
    Revoked,
};

struct ComponentActivationAuthorityLiveStateV1
{
    component_activation::ComponentActivationAuthorizationV1 record;
    std::optional<
        component_activation::ComponentActivationImmutableResourceGrantV1>
        resourceGrant;
    std::vector<std::uint8_t> resourceBytes;
    std::optional<
        component_activation::ComponentActivationAudioStreamGrantV1>
        audioStreamGrant;
    std::shared_ptr<
        const ComponentActivationFrozenAudioSourceDataV1> audioSource;
    std::optional<
        component_activation::ComponentActivationResourceSetGrantV1>
        resourceSetGrant;
    std::array<
        component_activation::ComponentActivationResourceSetMemberV1,
        component_activation::maximumComponentActivationResourceSetMembersV1>
        resourceSetMembers{};
    std::array<
        std::optional<
            component_activation::ComponentActivationImmutableResourceGrantV1>,
        component_activation::maximumComponentActivationResourceSetMembersV1>
        resourceSetImmutableGrants{};
    std::array<
        std::vector<std::uint8_t>,
        component_activation::maximumComponentActivationResourceSetMembersV1>
        resourceSetImmutableBytes{};
    std::array<
        std::optional<
            component_activation::ComponentActivationAudioStreamGrantV1>,
        component_activation::maximumComponentActivationResourceSetMembersV1>
        resourceSetAudioGrants{};
    std::array<
        std::shared_ptr<const ComponentActivationFrozenAudioSourceDataV1>,
        component_activation::maximumComponentActivationResourceSetMembersV1>
        resourceSetAudioSources{};
    std::uint16_t resourceSetMemberCount = 0;
    ComponentOwnerActivationUseV1 ownerUse;
    ComponentProbeLivenessV1 probeLiveness;
    std::shared_ptr<const ComponentActivationRuntimeLiveStateV1> runtime;
    std::shared_ptr<ComponentActivationStageGateV1> stageGate;
    std::atomic<ComponentActivationAuthorityStatusV1> status{
        ComponentActivationAuthorityStatusV1::Pending};
};
}

namespace talas::runtime::component_activation
{
namespace
{
template <typename Value, std::size_t Size>
[[nodiscard]] constexpr bool allZero(
    const std::array<Value, Size>& values) noexcept
{
    for (const auto value : values)
    {
        if (value != Value{})
            return false;
    }
    return true;
}

[[nodiscard]] bool exactStaticParameters(
    const component_bundle::ComponentBundleNode& node,
    std::span<const ComponentActivationStaticParameterV1> parameters) noexcept
{
    if (parameters.size() != node.parameters.size()
        || validateComponentActivationStaticParametersV1(parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& expected = node.parameters[index];
        const auto& parameter = parameters[index];
        if (std::string_view{parameter.id.data(), parameter.idBytes}
            != expected.id)
        {
            return false;
        }
        const auto kind = static_cast<ComponentActivationStaticParameterKindV1>(
            parameter.kind);
        switch (expected.kind)
        {
            case talas::core::ParameterKind::Expression:
                if (kind != ComponentActivationStaticParameterKindV1::Scalar)
                    return false;
                break;
            case talas::core::ParameterKind::Choice:
                if (kind != ComponentActivationStaticParameterKindV1::Integer
                    || parameter.integerValue < 0
                    || static_cast<std::uint64_t>(parameter.integerValue)
                        >= expected.choices.size())
                {
                    return false;
                }
                break;
            case talas::core::ParameterKind::Boolean:
                if (kind != ComponentActivationStaticParameterKindV1::Integer
                    || (parameter.integerValue != 0
                        && parameter.integerValue != 1))
                {
                    return false;
                }
                break;
            case talas::core::ParameterKind::Text:
                if (kind != ComponentActivationStaticParameterKindV1::Text)
                    return false;
                break;
            case talas::core::ParameterKind::Resource:
                if (kind
                        != ComponentActivationStaticParameterKindV1::Resource
                    || parameter.textBytes == 0)
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

[[nodiscard]] std::optional<
    std::array<std::uint8_t, componentTrustDigestBytes>>
parseCanonicalDigest(std::string_view hexadecimal) noexcept
{
    if (hexadecimal.size() != componentTrustDigestBytes * 2U)
        return std::nullopt;
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        return std::nullopt;
    };
    std::array<std::uint8_t, componentTrustDigestBytes> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto high = nibble(hexadecimal[index * 2U]);
        const auto low = nibble(hexadecimal[index * 2U + 1U]);
        if (!high.has_value() || !low.has_value())
            return std::nullopt;
        result[index] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(*high << 4U) | *low);
    }
    return result;
}

[[nodiscard]] const component_bundle::ComponentBundleFrozenPayload*
exactEntry(const component_bundle::ComponentBundleSnapshot& snapshot) noexcept
{
    const component_bundle::ComponentBundleFrozenPayload* entry = nullptr;
    for (const auto& payload : snapshot.payloads)
    {
        if (!payload.isEntry)
            continue;
        if (entry != nullptr)
            return nullptr;
        entry = &payload;
    }
    if (entry == nullptr || snapshot.payloads.size() != 1
        || entry->path != snapshot.manifest.entry.path
        || entry->sha256 != snapshot.manifest.entry.sha256
        || entry->bytes.size() != snapshot.manifest.entry.byteCount
        || entry->bytes.empty())
    {
        return nullptr;
    }
    return entry;
}

[[nodiscard]] bool live(
    const detail::ComponentActivationAuthorityLiveStateV1& state) noexcept
{
    return state.ownerUse.active() && state.probeLiveness.active()
        && state.runtime
        && state.runtime->active.load(std::memory_order_acquire);
}

[[nodiscard]] ComponentActivationAuthorizationResultV1 rejected(
    ComponentActivationAuthorizationCodeV1 code) noexcept
{
    return {std::nullopt, code};
}

[[nodiscard]] constexpr std::optional<std::uint64_t> resourceTransportBytes(
    std::uint64_t sourceBytes) noexcept
{
    constexpr auto page =
        maximumComponentActivationResourceSetTransportPageBytesV1;
    if (sourceBytes == 0
        || sourceBytes > std::numeric_limits<std::uint64_t>::max()
                - (page - 1U))
    {
        return std::nullopt;
    }
    return ((sourceBytes + page - 1U) / page) * page;
}

template <typename Record>
[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
recordDigest(const Record& record) noexcept
{
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&record),
            sizeof(record)});
}
}

ComponentActivationRuntimeSessionV1::ComponentActivationRuntimeSessionV1(
    std::shared_ptr<detail::ComponentActivationRuntimeLiveStateV1> state)
    noexcept
    : state_(std::move(state))
{
}

ComponentActivationRuntimeSessionV1::~ComponentActivationRuntimeSessionV1()
{
    revoke();
}

ComponentActivationRuntimeSessionV1::ComponentActivationRuntimeSessionV1(
    ComponentActivationRuntimeSessionV1&&) noexcept = default;

ComponentActivationRuntimeSessionV1&
ComponentActivationRuntimeSessionV1::operator=(
    ComponentActivationRuntimeSessionV1&& other) noexcept
{
    if (this != &other)
    {
        revoke();
        state_ = std::move(other.state_);
    }
    return *this;
}

void ComponentActivationRuntimeSessionV1::revoke() noexcept
{
    if (state_)
    {
        const std::unique_lock stageLock(state_->stageGate->mutex);
        state_->active.store(false, std::memory_order_release);
    }
}

ComponentActivationRuntimeEvidenceV1::ComponentActivationRuntimeEvidenceV1(
    std::weak_ptr<const detail::ComponentActivationRuntimeLiveStateV1> state)
    noexcept
    : state_(std::move(state))
{
}

bool ComponentActivationRuntimeEvidenceV1::active() const noexcept
{
    const auto state = state_.lock();
    return state && state->active.load(std::memory_order_acquire);
}

ComponentActivationStageLeaseV1::ComponentActivationStageLeaseV1(
    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state)
    : state_(std::move(state))
    , authorityLock_(state_->stageGate->mutex)
    , ownerLock_(state_->ownerUse.stageGate_->mutex)
    , probeLock_(state_->probeLiveness.stageGate_->mutex)
    , runtimeLock_(state_->runtime->stageGate->mutex)
{
}

ComponentActivationStageLeaseV1::~ComponentActivationStageLeaseV1() = default;

ComponentActivationStageLeaseV1::ComponentActivationStageLeaseV1(
    ComponentActivationStageLeaseV1&&) noexcept = default;

ComponentActivationStageLeaseV1&
ComponentActivationStageLeaseV1::operator=(
    ComponentActivationStageLeaseV1&& other) noexcept
{
    if (this != &other)
    {
        ComponentActivationStageLeaseV1 previous(std::move(other));
        state_.swap(previous.state_);
        authorityLock_.swap(previous.authorityLock_);
        ownerLock_.swap(previous.ownerLock_);
        probeLock_.swap(previous.probeLock_);
        runtimeLock_.swap(previous.runtimeLock_);
    }
    return *this;
}

bool ComponentActivationStageLeaseV1::active() const noexcept
{
    return state_
        && state_->status.load(std::memory_order_acquire)
            == detail::ComponentActivationAuthorityStatusV1::Consumed
        && live(*state_);
}

ComponentActivationLeaseV1::ComponentActivationLeaseV1(
    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state)
    noexcept
    : state_(std::move(state))
{
}

ComponentActivationLeaseV1::~ComponentActivationLeaseV1()
{
    revoke();
}

ComponentActivationLeaseV1::ComponentActivationLeaseV1(
    ComponentActivationLeaseV1&&) noexcept = default;

ComponentActivationLeaseV1& ComponentActivationLeaseV1::operator=(
    ComponentActivationLeaseV1&& other) noexcept
{
    if (this != &other)
    {
        revoke();
        state_ = std::move(other.state_);
    }
    return *this;
}

bool ComponentActivationLeaseV1::active() const noexcept
{
    return state_
        && state_->status.load(std::memory_order_acquire)
            == detail::ComponentActivationAuthorityStatusV1::Consumed
        && live(*state_);
}

const ComponentActivationAuthorizationV1*
ComponentActivationLeaseV1::record() const noexcept
{
    return active() ? &state_->record : nullptr;
}

const ComponentActivationImmutableResourceGrantV1*
ComponentActivationLeaseV1::resourceGrant() const noexcept
{
    return active() && state_->resourceGrant.has_value()
        ? &*state_->resourceGrant
        : nullptr;
}

std::span<const std::uint8_t>
ComponentActivationLeaseV1::resourceBytes() const noexcept
{
    return resourceGrant() != nullptr
        ? std::span<const std::uint8_t>{state_->resourceBytes}
        : std::span<const std::uint8_t>{};
}

const ComponentActivationAudioStreamGrantV1*
ComponentActivationLeaseV1::audioStreamGrant() const noexcept
{
    return active() && state_->audioStreamGrant.has_value()
        ? &*state_->audioStreamGrant
        : nullptr;
}

std::span<const std::uint8_t>
ComponentActivationLeaseV1::audioSourceBytes() const noexcept
{
    return audioStreamGrant() != nullptr && state_->audioSource
        ? std::span<const std::uint8_t>{state_->audioSource->bytes}
        : std::span<const std::uint8_t>{};
}

const ComponentActivationResourceSetGrantV1*
ComponentActivationLeaseV1::resourceSetGrant() const noexcept
{
    return active() && state_->resourceSetGrant.has_value()
        ? &*state_->resourceSetGrant
        : nullptr;
}

std::span<const ComponentActivationResourceSetMemberV1>
ComponentActivationLeaseV1::resourceSetMembers() const noexcept
{
    return resourceSetGrant() != nullptr
        ? std::span<const ComponentActivationResourceSetMemberV1>{
              state_->resourceSetMembers.data(),
              state_->resourceSetMemberCount}
        : std::span<const ComponentActivationResourceSetMemberV1>{};
}

const ComponentActivationImmutableResourceGrantV1*
ComponentActivationLeaseV1::resourceSetImmutableGrant(
    std::size_t index) const noexcept
{
    return resourceSetGrant() != nullptr
            && index < state_->resourceSetMemberCount
            && state_->resourceSetImmutableGrants[index].has_value()
        ? &*state_->resourceSetImmutableGrants[index]
        : nullptr;
}

std::span<const std::uint8_t>
ComponentActivationLeaseV1::resourceSetImmutableBytes(
    std::size_t index) const noexcept
{
    return resourceSetImmutableGrant(index) != nullptr
        ? std::span<const std::uint8_t>{
              state_->resourceSetImmutableBytes[index]}
        : std::span<const std::uint8_t>{};
}

const ComponentActivationAudioStreamGrantV1*
ComponentActivationLeaseV1::resourceSetAudioGrant(
    std::size_t index) const noexcept
{
    return resourceSetGrant() != nullptr
            && index < state_->resourceSetMemberCount
            && state_->resourceSetAudioGrants[index].has_value()
        ? &*state_->resourceSetAudioGrants[index]
        : nullptr;
}

std::span<const std::uint8_t>
ComponentActivationLeaseV1::resourceSetAudioBytes(
    std::size_t index) const noexcept
{
    return resourceSetAudioGrant(index) != nullptr
            && state_->resourceSetAudioSources[index]
        ? std::span<const std::uint8_t>{
              state_->resourceSetAudioSources[index]->bytes}
        : std::span<const std::uint8_t>{};
}

std::optional<ComponentActivationStageLeaseV1>
ComponentActivationLeaseV1::tryAcquireStageLease() noexcept
{
    if (!state_)
        return std::nullopt;
    try
    {
        ComponentActivationStageLeaseV1 stageLease{state_};
        if (!stageLease.active())
            return std::nullopt;
        return std::optional<ComponentActivationStageLeaseV1>{
            std::move(stageLease)};
    }
    catch (...)
    {
        return std::nullopt;
    }
}

void ComponentActivationLeaseV1::revoke() noexcept
{
    if (state_)
    {
        const std::unique_lock stageLock(state_->stageGate->mutex);
        state_->status.store(
            detail::ComponentActivationAuthorityStatusV1::Revoked,
            std::memory_order_release);
    }
}

ComponentActivationAuthorityV1::ComponentActivationAuthorityV1(
    std::shared_ptr<detail::ComponentActivationAuthorityLiveStateV1> state)
    noexcept
    : state_(std::move(state))
{
}

ComponentActivationAuthorityV1::~ComponentActivationAuthorityV1()
{
    revoke();
}

ComponentActivationAuthorityV1::ComponentActivationAuthorityV1(
    ComponentActivationAuthorityV1&&) noexcept = default;

ComponentActivationAuthorityV1& ComponentActivationAuthorityV1::operator=(
    ComponentActivationAuthorityV1&& other) noexcept
{
    if (this != &other)
    {
        revoke();
        state_ = std::move(other.state_);
    }
    return *this;
}

bool ComponentActivationAuthorityV1::pending() const noexcept
{
    return state_
        && state_->status.load(std::memory_order_acquire)
            == detail::ComponentActivationAuthorityStatusV1::Pending
        && live(*state_);
}

std::optional<ComponentActivationLeaseV1>
ComponentActivationAuthorityV1::consume() noexcept
{
    if (!state_ || !live(*state_))
    {
        revoke();
        return std::nullopt;
    }
    auto expected = detail::ComponentActivationAuthorityStatusV1::Pending;
    if (!state_->status.compare_exchange_strong(
            expected,
            detail::ComponentActivationAuthorityStatusV1::Consumed,
            std::memory_order_acq_rel,
            std::memory_order_acquire)
        || !live(*state_))
    {
        revoke();
        return std::nullopt;
    }
    auto lease = ComponentActivationLeaseV1{std::move(state_)};
    return std::optional<ComponentActivationLeaseV1>{std::move(lease)};
}

void ComponentActivationAuthorityV1::revoke() noexcept
{
    if (!state_)
        return;
    const std::unique_lock stageLock(state_->stageGate->mutex);
    state_->status.store(
        detail::ComponentActivationAuthorityStatusV1::Revoked,
        std::memory_order_release);
}

std::optional<ComponentActivationRuntimeEvidenceAuthoringV1>
ComponentActivationAuthorizationFactory::authorRuntimeEvidence(
    const ComponentActivationSealedSessionV1& session) noexcept
{
    if (validateComponentActivationSealedSessionV1(session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return std::nullopt;
    }
    try
    {
        auto state = std::make_shared<
            detail::ComponentActivationRuntimeLiveStateV1>();
        state->session = session;
        state->stageGate = std::make_shared<
            detail::ComponentActivationStageGateV1>();
        return ComponentActivationRuntimeEvidenceAuthoringV1{
            ComponentActivationRuntimeSessionV1{state},
            ComponentActivationRuntimeEvidenceV1{state},
        };
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
}

ComponentActivationAuthorizationResultV1
ComponentActivationAuthorizationFactory::make(
    const ComponentActivationAuthorizationInputsV1& inputs) noexcept
try
{
    if (inputs.snapshot == nullptr
        || !component_bundle::validateComponentBundleSnapshotIdentity(
            *inputs.snapshot))
    {
        return rejected(ComponentActivationAuthorizationCodeV1::SnapshotInvalid);
    }
    const auto& snapshot = *inputs.snapshot;
    const auto signature = inputs.signature.inspect();
    if (signature.state != ComponentSignatureState::Verified
        || signature.profileVersion != currentComponentSignatureProfileVersion
        || !signature.hasContentDigest || !signature.hasSignerIdentity
        || signature.contentDigest != snapshot.contentDigest
        || allZero(signature.contentDigest)
        || allZero(signature.signerIdentityDigest))
    {
        return rejected(ComponentActivationAuthorizationCodeV1::SignatureInvalid);
    }
    if (inputs.descriptorId.empty()
        || inputs.descriptorId.size()
            > maximumComponentActivationDescriptorIdBytesV1
        || (snapshot.manifest.nodes.size() == 1
            && snapshot.manifest.nodes.front().typeId
                != inputs.descriptorId))
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::DescriptorInvalid);
    }
    const auto manifestProfile = supportedComponentActivationManifestProfileV1(
            snapshot,
            inputs.inputChannelCount,
            inputs.outputChannelCount,
            inputs.opaqueStateByteCount);
    if (manifestProfile == ComponentActivationManifestProfileV1::Unsupported)
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::ProfileUnsupported);
    }
    if (!exactStaticParameters(
            snapshot.manifest.nodes.front(), inputs.staticParameters))
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::ProfileUnsupported);
    }
    const auto* entry = exactEntry(snapshot);
    if (entry == nullptr)
        return rejected(ComponentActivationAuthorizationCodeV1::EntryInvalid);
    if (inputs.artifactGeneration == 0 || inputs.activationGeneration == 0
        || allZero(inputs.validationNonce)
        || allZero(inputs.authorizationNonce))
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::SessionBindingInvalid);
    }

    std::array<
        const ComponentActivationStaticParameterV1*,
        maximumComponentActivationResourceSetMembersV1>
        resourceParameters{};
    std::uint16_t resourceParameterCount = 0;
    for (const auto& parameter : inputs.staticParameters)
    {
        if (parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource))
        {
            if (resourceParameterCount >= resourceParameters.size())
            {
                return rejected(
                    ComponentActivationAuthorizationCodeV1::
                        ProfileUnsupported);
            }
            resourceParameters[resourceParameterCount++] = &parameter;
        }
    }
    const auto* resourceParameter = resourceParameterCount == 0
        ? nullptr
        : resourceParameters.front();
    talas::core::ResourceSnapshotPtr resourceSnapshot;
    std::optional<std::array<std::uint8_t, componentTrustDigestBytes>>
        sourceContentDigest;
    std::vector<std::uint8_t> resourceBytes;
    std::array<std::uint8_t, componentTrustDigestBytes>
        mediatedPayloadDigest{};
    std::uint64_t resourceElementCount = 0;
    std::uint32_t resourceKind = 0;
    std::uint32_t resourceElementFormat = 0;
    std::shared_ptr<
        const detail::ComponentActivationFrozenAudioSourceDataV1> audioSource;
    std::array<
        talas::core::ResourceSnapshotPtr,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetSnapshots{};
    std::array<
        std::shared_ptr<
            const detail::ComponentActivationFrozenAudioSourceDataV1>,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetAudioSources{};
    std::array<
        std::array<std::uint8_t, componentTrustDigestBytes>,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetSourceDigests{};
    std::array<
        ComponentActivationResourceSetMemberKindV1,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetKinds{};
    std::array<
        std::uint64_t,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetSourceBytes{};
    std::array<
        std::uint64_t,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetTransportBytes{};
    ComponentActivationResourceSetBudgetV1 resourceSetBudget;
    if (manifestProfile
        == ComponentActivationManifestProfileV1::MultipleImmutableResources)
    {
        if (inputs.resourceSnapshot || inputs.audioSource != nullptr
            || inputs.resourceSetInputs.size() != resourceParameterCount)
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        for (std::size_t index = 0;
             index < inputs.resourceSetInputs.size();
             ++index)
        {
            const auto& input = inputs.resourceSetInputs[index];
            const auto* parameter = resourceParameters[index];
            const auto& candidateSnapshot = input.snapshot;
            if (!candidateSnapshot || parameter == nullptr
                || candidateSnapshot->id
                    != std::string_view{
                        parameter->text.data(), parameter->textBytes}
                || candidateSnapshot->generation == 0)
            {
                return rejected(
                    ComponentActivationAuthorizationCodeV1::ResourceInvalid);
            }
            for (std::size_t prior = 0; prior < index; ++prior)
            {
                if (resourceSetSnapshots[prior]->id == candidateSnapshot->id)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
            }
            const auto sourceDigest = parseCanonicalDigest(
                candidateSnapshot->contentHash);
            if (!sourceDigest.has_value() || allZero(*sourceDigest))
            {
                return rejected(
                    ComponentActivationAuthorizationCodeV1::ResourceInvalid);
            }

            auto sourceBytes = std::uint64_t{0};
            auto workingBytes = std::uint64_t{0};
            switch (candidateSnapshot->kind)
            {
                case talas::core::ResourceSnapshotKind::Table:
                    if (input.audioSource != nullptr
                        || candidateSnapshot->tableSamples.empty()
                        || !candidateSnapshot->tableRightSamples.empty()
                        || candidateSnapshot->tableSamples.size()
                            > maximumComponentActivationImmutableTableElementsV1
                        || !candidateSnapshot->binaryBytes.empty()
                        || !candidateSnapshot->audioBytes.empty()
                        || candidateSnapshot->channelCount != 0
                        || candidateSnapshot->sampleRate != 0.0
                        || candidateSnapshot->frameCount != 0
                        || !candidateSnapshot->encoding.empty()
                        || !std::ranges::all_of(
                            candidateSnapshot->tableSamples,
                            [](double value) {
                                return std::isfinite(value);
                            }))
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                ResourceInvalid);
                    }
                    sourceBytes = candidateSnapshot->tableSamples.size()
                        * sizeof(double);
                    resourceSetKinds[index] =
                        ComponentActivationResourceSetMemberKindV1::Table;
                    break;
                case talas::core::ResourceSnapshotKind::Binary:
                    if (input.audioSource != nullptr
                        || candidateSnapshot->binaryBytes.empty()
                        || candidateSnapshot->binaryBytes.size()
                            > maximumComponentActivationImmutableBinaryBytesV1
                        || !candidateSnapshot->tableSamples.empty()
                        || !candidateSnapshot->tableRightSamples.empty()
                        || !candidateSnapshot->audioBytes.empty()
                        || candidateSnapshot->channelCount != 0
                        || candidateSnapshot->sampleRate != 0.0
                        || candidateSnapshot->frameCount != 0
                        || !candidateSnapshot->encoding.empty()
                        || calculateComponentActivationByteDigestV1(
                            candidateSnapshot->binaryBytes)
                            != *sourceDigest)
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                ResourceInvalid);
                    }
                    sourceBytes = candidateSnapshot->binaryBytes.size();
                    resourceSetKinds[index] =
                        ComponentActivationResourceSetMemberKindV1::Binary;
                    break;
                case talas::core::ResourceSnapshotKind::Audio:
                {
                    if (input.audioSource == nullptr
                        || !input.audioSource->valid())
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                AudioSourceInvalid);
                    }
                    const auto source = input.audioSource->data_;
                    if (!source || source->id != candidateSnapshot->id
                        || source->generation
                            != candidateSnapshot->generation
                        || source->sourceContentDigest != *sourceDigest
                        || calculateComponentActivationByteDigestV1(
                            source->bytes)
                            != source->sourceContentDigest
                        || source->bytes.size()
                            < minimumComponentActivationAudioSourceBytesV1
                        || source->bytes.size()
                            > maximumComponentActivationAudioSourceBytesV1
                        || !candidateSnapshot->tableSamples.empty()
                        || !candidateSnapshot->tableRightSamples.empty()
                        || !candidateSnapshot->binaryBytes.empty()
                        || (!candidateSnapshot->audioBytes.empty()
                            && candidateSnapshot->audioBytes != source->bytes)
                        || candidateSnapshot->channelCount
                            != source->channelCount
                        || candidateSnapshot->sampleRate != source->sampleRate
                        || candidateSnapshot->frameCount != source->frameCount
                        || candidateSnapshot->encoding
                            != talas::project::pcm16WavAssetEncoding)
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                AudioSourceInvalid);
                    }
                    sourceBytes = source->bytes.size();
                    workingBytes =
                        maximumComponentActivationAudioStreamWorkingBytesV1;
                    resourceSetAudioSources[index] = source;
                    resourceSetKinds[index] =
                        ComponentActivationResourceSetMemberKindV1::Audio;
                    break;
                }
                default:
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
            }
            const auto transportBytes =
                resourceTransportBytes(sourceBytes);
            if (!transportBytes.has_value()
                || tryAppendComponentActivationResourceSetBudgetMemberV1(
                    resourceSetBudget,
                    {
                        .sourceByteCount = sourceBytes,
                        .transportByteCount = *transportBytes,
                        .privateMappedByteCount = *transportBytes,
                        .workingByteCount = workingBytes,
                        .audioStream = resourceSetKinds[index]
                            == ComponentActivationResourceSetMemberKindV1::
                                Audio,
                    })
                    != ComponentActivationResourceSetBudgetDiagnosticV1::Valid)
            {
                return rejected(
                    ComponentActivationAuthorizationCodeV1::ResourceInvalid);
            }
            resourceSetSnapshots[index] = candidateSnapshot;
            resourceSetSourceDigests[index] = *sourceDigest;
            resourceSetSourceBytes[index] = sourceBytes;
            resourceSetTransportBytes[index] = *transportBytes;
        }
    }
    else if (manifestProfile
        == ComponentActivationManifestProfileV1::ImmutableResource)
    {
        resourceSnapshot = inputs.resourceSnapshot;
        if (resourceParameter == nullptr || !resourceSnapshot
            || inputs.audioSource != nullptr
            || resourceSnapshot->id
                != std::string_view{
                    resourceParameter->text.data(),
                    resourceParameter->textBytes}
            || resourceSnapshot->generation == 0
            || resourceSnapshot->channelCount != 0
            || resourceSnapshot->sampleRate != 0.0
            || resourceSnapshot->frameCount != 0
            || !resourceSnapshot->encoding.empty())
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        sourceContentDigest = parseCanonicalDigest(
            resourceSnapshot->contentHash);
        if (!sourceContentDigest.has_value())
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        switch (resourceSnapshot->kind)
        {
            case talas::core::ResourceSnapshotKind::Table:
                if (resourceSnapshot->tableSamples.empty()
                    || !resourceSnapshot->tableRightSamples.empty()
                    || resourceSnapshot->tableSamples.size()
                        > maximumComponentActivationImmutableTableElementsV1
                    || !resourceSnapshot->binaryBytes.empty()
                    || !resourceSnapshot->audioBytes.empty())
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
                try
                {
                    resourceBytes.resize(
                        resourceSnapshot->tableSamples.size()
                        * sizeof(double));
                }
                catch (const std::bad_alloc&)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            AllocationFailed);
                }
                if (encodeComponentActivationImmutableTableV1(
                        resourceSnapshot->tableSamples, resourceBytes)
                    != ComponentActivationProtocolDiagnosticV1::Valid)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
                resourceElementCount = resourceSnapshot->tableSamples.size();
                resourceKind = static_cast<std::uint32_t>(
                    ComponentActivationImmutableResourceKindV1::Table);
                resourceElementFormat = static_cast<std::uint32_t>(
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
                break;
            case talas::core::ResourceSnapshotKind::Binary:
                if (resourceSnapshot->binaryBytes.empty()
                    || resourceSnapshot->binaryBytes.size()
                        > maximumComponentActivationImmutableBinaryBytesV1
                    || !resourceSnapshot->tableSamples.empty()
                    || !resourceSnapshot->tableRightSamples.empty()
                    || !resourceSnapshot->audioBytes.empty())
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
                try
                {
                    resourceBytes.resize(
                        resourceSnapshot->binaryBytes.size());
                }
                catch (const std::bad_alloc&)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            AllocationFailed);
                }
                if (copyComponentActivationImmutableBinaryV1(
                        resourceSnapshot->binaryBytes, resourceBytes)
                    != ComponentActivationProtocolDiagnosticV1::Valid)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
                resourceElementCount = resourceSnapshot->binaryBytes.size();
                resourceKind = static_cast<std::uint32_t>(
                    ComponentActivationImmutableResourceKindV1::Binary);
                resourceElementFormat = static_cast<std::uint32_t>(
                    ComponentActivationImmutableResourceElementFormatV1::
                        RawBytes);
                break;
            case talas::core::ResourceSnapshotKind::Audio:
            default:
                return rejected(
                    ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        mediatedPayloadDigest = calculateComponentActivationByteDigestV1(
            resourceBytes);
        if (allZero(mediatedPayloadDigest)
            || (resourceSnapshot->kind
                    == talas::core::ResourceSnapshotKind::Binary
                && mediatedPayloadDigest != *sourceContentDigest))
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
    }
    else if (manifestProfile
        == ComponentActivationManifestProfileV1::BoundedAudioStream)
    {
        resourceSnapshot = inputs.resourceSnapshot;
        if (resourceParameter == nullptr || !resourceSnapshot
            || inputs.audioSource == nullptr
            || !inputs.audioSource->valid())
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::AudioSourceInvalid);
        }
        audioSource = inputs.audioSource->data_;
        sourceContentDigest = parseCanonicalDigest(
            resourceSnapshot->contentHash);
        if (!audioSource || !sourceContentDigest.has_value()
            || resourceSnapshot->id
                != std::string_view{
                    resourceParameter->text.data(),
                    resourceParameter->textBytes}
            || resourceSnapshot->id != audioSource->id
            || resourceSnapshot->generation == 0
            || resourceSnapshot->generation != audioSource->generation
            || resourceSnapshot->kind
                != talas::core::ResourceSnapshotKind::Audio
            || !resourceSnapshot->tableSamples.empty()
            || !resourceSnapshot->tableRightSamples.empty()
            || !resourceSnapshot->binaryBytes.empty()
            || (!resourceSnapshot->audioBytes.empty()
                && resourceSnapshot->audioBytes != audioSource->bytes)
            || resourceSnapshot->channelCount
                != audioSource->channelCount
            || resourceSnapshot->sampleRate != audioSource->sampleRate
            || resourceSnapshot->frameCount != audioSource->frameCount
            || resourceSnapshot->encoding
                != talas::project::pcm16WavAssetEncoding
            || audioSource->bytes.size()
                < minimumComponentActivationAudioSourceBytesV1
            || audioSource->bytes.size()
                > maximumComponentActivationAudioSourceBytesV1
            || audioSource->sourceContentDigest != *sourceContentDigest
            || calculateComponentActivationByteDigestV1(audioSource->bytes)
                != audioSource->sourceContentDigest)
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::AudioSourceInvalid);
        }
    }
    else if (inputs.resourceSnapshot || inputs.audioSource != nullptr
        || !inputs.resourceSetInputs.empty() || resourceParameter != nullptr)
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::ResourceInvalid);
    }

    ComponentApprovalFingerprint candidate;
    candidate.bundleId = snapshot.manifest.bundleId;
    candidate.semanticVersion = snapshot.manifest.semanticVersion;
    candidate.contentDigest = snapshot.contentDigest;
    candidate.signerIdentityDigest = signature.signerIdentityDigest;
    candidate.requestedPermissions = snapshot.manifest.requestedPermissions;
    candidate.policyVersion = currentComponentTrustPolicyVersion;
    candidate.hasContentDigest = true;
    candidate.hasSignerIdentity = true;

    ComponentTrustEvidence trust;
    trust.candidate = candidate;
    trust.ownerAuthorizationEvidence = inputs.ownerAuthorization;
    trust.signature = inputs.signature;
    trust.probe = inputs.probe;
    trust.validationNonce = inputs.validationNonce;
    trust.artifactGeneration = inputs.artifactGeneration;
    trust.declaredRoles = snapshot.manifest.declaredRoles;
    trust.hasValidationNonce = true;
    const auto trustDecision = evaluateComponentTrust(trust);
    if (trustDecision
        != ComponentTrustDecision{
            ComponentTrustDisposition::PreserveOnly,
            ComponentTrustDiagnosticCode::IsolationUnavailable})
    {
        switch (trustDecision.diagnostic)
        {
            case ComponentTrustDiagnosticCode::ApprovalMissing:
            case ComponentTrustDiagnosticCode::ApprovalStale:
            case ComponentTrustDiagnosticCode::ApprovalDenied:
            case ComponentTrustDiagnosticCode::ApprovalInvalid:
                return rejected(
                    ComponentActivationAuthorizationCodeV1::OwnerNotApproved);
            case ComponentTrustDiagnosticCode::ProbeRequired:
            case ComponentTrustDiagnosticCode::ProbeRejected:
            case ComponentTrustDiagnosticCode::ProbeControlledFatal:
            case ComponentTrustDiagnosticCode::ProbeCrashed:
            case ComponentTrustDiagnosticCode::ProbeTimedOut:
            case ComponentTrustDiagnosticCode::ProbeProtocolError:
            case ComponentTrustDiagnosticCode::ProbeEvidenceContradictory:
            case ComponentTrustDiagnosticCode::ProbeEvidenceMismatch:
            case ComponentTrustDiagnosticCode::ProbeProfileUnsupported:
            case ComponentTrustDiagnosticCode::ProbeStale:
                return rejected(
                    ComponentActivationAuthorizationCodeV1::ProbeInvalid);
            default:
                return rejected(
                    ComponentActivationAuthorizationCodeV1::SignatureInvalid);
        }
    }

    auto runtime = inputs.runtime.state_.lock();
    if (!runtime || !runtime->active.load(std::memory_order_acquire)
        || validateComponentActivationSealedSessionV1(runtime->session)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return rejected(ComponentActivationAuthorizationCodeV1::RuntimeInvalid);
    }

    ComponentActivationAuthorizationV1 record;
    record.header = componentActivationHeaderV1(
        ComponentActivationRecordKindV1::Authorization,
        sizeof(record));
    record.bundleIdBytes = static_cast<std::uint32_t>(
        snapshot.manifest.bundleId.size());
    record.semanticVersionBytes = static_cast<std::uint32_t>(
        snapshot.manifest.semanticVersion.size());
    record.descriptorIdBytes = static_cast<std::uint32_t>(
        inputs.descriptorId.size());
    record.sampleRate = inputs.sampleRate;
    std::copy(
        snapshot.manifest.bundleId.begin(),
        snapshot.manifest.bundleId.end(),
        record.bundleId.begin());
    std::copy(
        snapshot.manifest.semanticVersion.begin(),
        snapshot.manifest.semanticVersion.end(),
        record.semanticVersion.begin());
    std::copy(
        inputs.descriptorId.begin(),
        inputs.descriptorId.end(),
        record.descriptorId.begin());
    record.contentDigest = snapshot.contentDigest;
    record.signerIdentityDigest = signature.signerIdentityDigest;
    record.entryDigest = entry->sha256;
    record.requestedPermissions = snapshot.manifest.requestedPermissions;
    record.declaredRoles = snapshot.manifest.declaredRoles;
    record.policyVersion = currentComponentTrustPolicyVersion;
    record.blockFrames = inputs.blockFrames;
    record.inputChannelCount = inputs.inputChannelCount;
    record.outputChannelCount = inputs.outputChannelCount;
    record.staticParameterCount = static_cast<std::uint16_t>(
        inputs.staticParameters.size());
    record.staticParameterDigest =
        calculateComponentActivationStaticParameterDigestV1(
            inputs.staticParameters);
    record.opaqueStateByteCount = inputs.opaqueStateByteCount;
    record.entryByteCount = entry->bytes.size();
    record.artifactGeneration = inputs.artifactGeneration;
    record.activationGeneration = inputs.activationGeneration;
    record.runtimeSessionGeneration =
        runtime->session.runtimeSessionGeneration;
    record.sharedStorageIdentity = runtime->session.sharedStorageIdentity;
    record.validationNonce = inputs.validationNonce;
    record.runtimeSessionNonce = runtime->session.runtimeSessionNonce;
    record.authorizationNonce = inputs.authorizationNonce;
    if (validateComponentActivationAuthorizationV1(
            record, runtime->session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return rejected(
            ComponentActivationAuthorizationCodeV1::ProfileUnsupported);
    }

    std::optional<ComponentActivationImmutableResourceGrantV1> resourceGrant;
    if (manifestProfile
        == ComponentActivationManifestProfileV1::ImmutableResource)
    {
        ComponentActivationImmutableResourceGrantV1 grant;
        grant.header = componentActivationHeaderV1(
            ComponentActivationRecordKindV1::ImmutableResourceGrant,
            sizeof(grant));
        grant.contentDigest = record.contentDigest;
        grant.sourceContentDigest = *sourceContentDigest;
        grant.mediatedPayloadDigest = mediatedPayloadDigest;
        grant.validationNonce = record.validationNonce;
        grant.runtimeSessionNonce = record.runtimeSessionNonce;
        grant.authorizationNonce = record.authorizationNonce;
        grant.artifactGeneration = record.artifactGeneration;
        grant.activationGeneration = record.activationGeneration;
        grant.runtimeSessionGeneration = record.runtimeSessionGeneration;
        grant.sharedStorageIdentity = record.sharedStorageIdentity;
        grant.resourceGeneration = resourceSnapshot->generation;
        grant.elementCount = resourceElementCount;
        grant.byteCount = resourceBytes.size();
        grant.parameterIdBytes = resourceParameter->idBytes;
        grant.resourceIdBytes = resourceParameter->textBytes;
        grant.resourceKind = resourceKind;
        grant.elementFormat = resourceElementFormat;
        std::copy_n(
            resourceParameter->id.begin(),
            grant.parameterIdBytes,
            grant.parameterId.begin());
        std::copy_n(
            resourceParameter->text.begin(),
            grant.resourceIdBytes,
            grant.resourceId.begin());
        if (validateComponentActivationImmutableResourceGrantV1(
                grant,
                record,
                runtime->session,
                inputs.staticParameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        resourceGrant = grant;
    }

    std::optional<ComponentActivationAudioStreamGrantV1> audioStreamGrant;
    if (manifestProfile
        == ComponentActivationManifestProfileV1::BoundedAudioStream)
    {
        ComponentActivationAudioStreamGrantV1 grant;
        grant.header = componentActivationHeaderV1(
            ComponentActivationRecordKindV1::AudioStreamGrant,
            sizeof(grant));
        grant.contentDigest = record.contentDigest;
        grant.signerIdentityDigest = record.signerIdentityDigest;
        grant.sourceContentDigest = audioSource->sourceContentDigest;
        grant.validationNonce = record.validationNonce;
        grant.runtimeSessionNonce = record.runtimeSessionNonce;
        grant.authorizationNonce = record.authorizationNonce;
        grant.artifactGeneration = record.artifactGeneration;
        grant.activationGeneration = record.activationGeneration;
        grant.runtimeSessionGeneration = record.runtimeSessionGeneration;
        grant.sharedStorageIdentity = record.sharedStorageIdentity;
        grant.resourceGeneration = audioSource->generation;
        grant.sourceByteCount = audioSource->bytes.size();
        grant.sourceFrameCount = audioSource->frameCount;
        grant.maximumSourceFrameIncrement =
            maximumComponentActivationAudioSourceFrameIncrementV1;
        grant.parameterIdBytes = resourceParameter->idBytes;
        grant.resourceIdBytes = resourceParameter->textBytes;
        grant.sourceSampleRate = audioSource->sampleRate;
        grant.streamProfileVersion =
            componentActivationAudioStreamProfileVersionV1;
        grant.sourceEncoding = static_cast<std::uint32_t>(
            ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
        grant.maximumReadFrames =
            maximumComponentActivationAudioReadFramesV1;
        grant.decodeBlockFrames =
            componentActivationAudioDecodeBlockFramesV1;
        grant.sourceChannelCount = audioSource->channelCount;
        grant.maximumTouchedSourceBlocks =
            maximumComponentActivationAudioTouchedSourceBlocksV1;
        grant.requestCapacity = componentActivationAudioRequestCapacityV1;
        grant.responseCapacity = componentActivationAudioResponseCapacityV1;
        grant.workerCacheCapacity =
            componentActivationAudioWorkerCacheCapacityV1;
        grant.lowWatermarkBlocks =
            componentActivationAudioLowWatermarkBlocksV1;
        grant.highWatermarkBlocks =
            componentActivationAudioHighWatermarkBlocksV1;
        std::copy_n(
            resourceParameter->id.begin(),
            grant.parameterIdBytes,
            grant.parameterId.begin());
        std::copy_n(
            resourceParameter->text.begin(),
            grant.resourceIdBytes,
            grant.resourceId.begin());
        if (validateComponentActivationAudioStreamGrantV1(
                grant,
                record,
                runtime->session,
                inputs.staticParameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::AudioSourceInvalid);
        }
        audioStreamGrant = grant;
    }

    std::optional<ComponentActivationResourceSetGrantV1> resourceSetGrant;
    std::array<
        ComponentActivationResourceSetMemberV1,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetMembers{};
    std::array<
        std::optional<ComponentActivationImmutableResourceGrantV1>,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetImmutableGrants{};
    std::array<
        std::vector<std::uint8_t>,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetImmutableBytes{};
    std::array<
        std::optional<ComponentActivationAudioStreamGrantV1>,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetAudioGrants{};
    if (manifestProfile
        == ComponentActivationManifestProfileV1::MultipleImmutableResources)
    {
        for (std::size_t index = 0; index < resourceParameterCount; ++index)
        {
            const auto* parameter = resourceParameters[index];
            const auto& setSnapshot = resourceSetSnapshots[index];
            auto& member = resourceSetMembers[index];
            member.header = componentActivationHeaderV1(
                ComponentActivationRecordKindV1::ResourceSetMember,
                sizeof(member));
            member.sourceContentDigest = resourceSetSourceDigests[index];
            member.resourceGeneration = setSnapshot->generation;
            member.sourceByteCount = resourceSetSourceBytes[index];
            member.transportByteCount = resourceSetTransportBytes[index];
            member.privateMappedByteCount = resourceSetTransportBytes[index];
            member.workingByteCount = resourceSetKinds[index]
                    == ComponentActivationResourceSetMemberKindV1::Audio
                ? maximumComponentActivationAudioStreamWorkingBytesV1
                : 0;
            member.parameterIdBytes = parameter->idBytes;
            member.resourceIdBytes = parameter->textBytes;
            member.memberKind = static_cast<std::uint32_t>(
                resourceSetKinds[index]);
            member.memberOrdinal = static_cast<std::uint16_t>(index);
            member.memberCount = resourceParameterCount;
            std::copy_n(
                parameter->id.begin(),
                member.parameterIdBytes,
                member.parameterId.begin());
            std::copy_n(
                parameter->text.begin(),
                member.resourceIdBytes,
                member.resourceId.begin());

            if (resourceSetKinds[index]
                != ComponentActivationResourceSetMemberKindV1::Audio)
            {
                auto& bytes = resourceSetImmutableBytes[index];
                try
                {
                    bytes.resize(resourceSetSourceBytes[index]);
                }
                catch (const std::bad_alloc&)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            AllocationFailed);
                }
                auto grant = ComponentActivationImmutableResourceGrantV1{};
                grant.header = componentActivationHeaderV1(
                    ComponentActivationRecordKindV1::ImmutableResourceGrant,
                    sizeof(grant));
                grant.contentDigest = record.contentDigest;
                grant.sourceContentDigest = resourceSetSourceDigests[index];
                grant.validationNonce = record.validationNonce;
                grant.runtimeSessionNonce = record.runtimeSessionNonce;
                grant.authorizationNonce = record.authorizationNonce;
                grant.artifactGeneration = record.artifactGeneration;
                grant.activationGeneration = record.activationGeneration;
                grant.runtimeSessionGeneration =
                    record.runtimeSessionGeneration;
                grant.sharedStorageIdentity = record.sharedStorageIdentity;
                grant.resourceGeneration = setSnapshot->generation;
                grant.parameterIdBytes = parameter->idBytes;
                grant.resourceIdBytes = parameter->textBytes;
                std::copy_n(
                    parameter->id.begin(),
                    grant.parameterIdBytes,
                    grant.parameterId.begin());
                std::copy_n(
                    parameter->text.begin(),
                    grant.resourceIdBytes,
                    grant.resourceId.begin());
                if (resourceSetKinds[index]
                    == ComponentActivationResourceSetMemberKindV1::Table)
                {
                    grant.elementCount = setSnapshot->tableSamples.size();
                    grant.byteCount = bytes.size();
                    grant.resourceKind = static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceKindV1::Table);
                    grant.elementFormat = static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceElementFormatV1::
                            Float64LittleEndian);
                    if (encodeComponentActivationImmutableTableV1(
                            setSnapshot->tableSamples, bytes)
                        != ComponentActivationProtocolDiagnosticV1::Valid)
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                ResourceInvalid);
                    }
                }
                else
                {
                    grant.elementCount = setSnapshot->binaryBytes.size();
                    grant.byteCount = bytes.size();
                    grant.resourceKind = static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceKindV1::Binary);
                    grant.elementFormat = static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceElementFormatV1::
                            RawBytes);
                    if (copyComponentActivationImmutableBinaryV1(
                            setSnapshot->binaryBytes, bytes)
                        != ComponentActivationProtocolDiagnosticV1::Valid)
                    {
                        return rejected(
                            ComponentActivationAuthorizationCodeV1::
                                ResourceInvalid);
                    }
                }
                grant.mediatedPayloadDigest =
                    calculateComponentActivationByteDigestV1(bytes);
                if (validateComponentActivationImmutableResourceGrantV1(
                        grant,
                        record,
                        runtime->session,
                        inputs.staticParameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            ResourceInvalid);
                }
                member.mediatedPayloadDigest =
                    grant.mediatedPayloadDigest;
                member.memberGrantDigest = recordDigest(grant);
                resourceSetImmutableGrants[index] = grant;
            }
            else
            {
                const auto& source = resourceSetAudioSources[index];
                auto grant = ComponentActivationAudioStreamGrantV1{};
                grant.header = componentActivationHeaderV1(
                    ComponentActivationRecordKindV1::AudioStreamGrant,
                    sizeof(grant));
                grant.contentDigest = record.contentDigest;
                grant.signerIdentityDigest = record.signerIdentityDigest;
                grant.sourceContentDigest = source->sourceContentDigest;
                grant.validationNonce = record.validationNonce;
                grant.runtimeSessionNonce = record.runtimeSessionNonce;
                grant.authorizationNonce = record.authorizationNonce;
                grant.artifactGeneration = record.artifactGeneration;
                grant.activationGeneration = record.activationGeneration;
                grant.runtimeSessionGeneration =
                    record.runtimeSessionGeneration;
                grant.sharedStorageIdentity = record.sharedStorageIdentity;
                grant.resourceGeneration = source->generation;
                grant.sourceByteCount = source->bytes.size();
                grant.sourceFrameCount = source->frameCount;
                grant.maximumSourceFrameIncrement =
                    maximumComponentActivationAudioSourceFrameIncrementV1;
                grant.parameterIdBytes = parameter->idBytes;
                grant.resourceIdBytes = parameter->textBytes;
                grant.sourceSampleRate = source->sampleRate;
                grant.streamProfileVersion =
                    componentActivationAudioStreamProfileVersionV1;
                grant.sourceEncoding = static_cast<std::uint32_t>(
                    ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
                grant.maximumReadFrames =
                    maximumComponentActivationAudioReadFramesV1;
                grant.decodeBlockFrames =
                    componentActivationAudioDecodeBlockFramesV1;
                grant.sourceChannelCount = source->channelCount;
                grant.maximumTouchedSourceBlocks =
                    maximumComponentActivationAudioTouchedSourceBlocksV1;
                grant.requestCapacity =
                    componentActivationAudioRequestCapacityV1;
                grant.responseCapacity =
                    componentActivationAudioResponseCapacityV1;
                grant.workerCacheCapacity =
                    componentActivationAudioWorkerCacheCapacityV1;
                grant.lowWatermarkBlocks =
                    componentActivationAudioLowWatermarkBlocksV1;
                grant.highWatermarkBlocks =
                    componentActivationAudioHighWatermarkBlocksV1;
                std::copy_n(
                    parameter->id.begin(),
                    grant.parameterIdBytes,
                    grant.parameterId.begin());
                std::copy_n(
                    parameter->text.begin(),
                    grant.resourceIdBytes,
                    grant.resourceId.begin());
                if (validateComponentActivationAudioStreamGrantV1(
                        grant,
                        record,
                        runtime->session,
                        inputs.staticParameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid)
                {
                    return rejected(
                        ComponentActivationAuthorizationCodeV1::
                            AudioSourceInvalid);
                }
                member.mediatedPayloadDigest = grant.sourceContentDigest;
                member.memberGrantDigest = recordDigest(grant);
                resourceSetAudioGrants[index] = grant;
            }
        }

        auto summary = ComponentActivationResourceSetGrantV1{};
        summary.header = componentActivationHeaderV1(
            ComponentActivationRecordKindV1::ResourceSetGrant,
            sizeof(summary));
        summary.contentDigest = record.contentDigest;
        summary.validationNonce = record.validationNonce;
        summary.runtimeSessionNonce = record.runtimeSessionNonce;
        summary.authorizationNonce = record.authorizationNonce;
        summary.artifactGeneration = record.artifactGeneration;
        summary.activationGeneration = record.activationGeneration;
        summary.runtimeSessionGeneration = record.runtimeSessionGeneration;
        summary.sharedStorageIdentity = record.sharedStorageIdentity;
        summary.aggregateSourceBytes =
            resourceSetBudget.aggregateSourceBytes;
        summary.aggregateTransportBytes =
            resourceSetBudget.aggregateTransportBytes;
        summary.aggregatePrivateMappedBytes =
            resourceSetBudget.aggregatePrivateMappedBytes;
        summary.aggregateWorkingBytes =
            resourceSetBudget.aggregateWorkingBytes;
        summary.profileVersion =
            componentActivationResourceSetProfileVersionV1;
        summary.memberCount = resourceSetBudget.memberCount;
        summary.audioStreamCount = resourceSetBudget.audioStreamCount;
        const auto memberSpan =
            std::span<const ComponentActivationResourceSetMemberV1>{
                resourceSetMembers.data(), resourceParameterCount};
        summary.resourceSetDigest =
            calculateComponentActivationResourceSetDigestV1(
                summary, memberSpan);
        for (std::size_t index = 0; index < resourceParameterCount; ++index)
            resourceSetMembers[index].resourceSetDigest =
                summary.resourceSetDigest;
        if (validateComponentActivationResourceSetGrantV1(
                summary, record, runtime->session)
                != ComponentActivationProtocolDiagnosticV1::Valid
            || validateComponentActivationResourceSetMembersV1(
                summary, memberSpan)
                != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            return rejected(
                ComponentActivationAuthorizationCodeV1::ResourceInvalid);
        }
        resourceSetGrant = summary;
    }

    auto ownerUse = inputs.ownerAuthorization.acquireExactActivationUse(
        candidate);
    if (!ownerUse.active())
        return rejected(ComponentActivationAuthorizationCodeV1::OwnerNotApproved);
    auto probeLiveness = inputs.probe.acquireLiveness();
    if (!probeLiveness.active())
        return rejected(ComponentActivationAuthorizationCodeV1::ProbeInvalid);
    if (!runtime->active.load(std::memory_order_acquire))
        return rejected(ComponentActivationAuthorizationCodeV1::RuntimeInvalid);

    try
    {
        auto state = std::make_shared<
            detail::ComponentActivationAuthorityLiveStateV1>();
        state->record = record;
        state->resourceGrant = resourceGrant;
        state->resourceBytes = std::move(resourceBytes);
        state->audioStreamGrant = audioStreamGrant;
        state->audioSource = std::move(audioSource);
        state->resourceSetGrant = resourceSetGrant;
        state->resourceSetMembers = resourceSetMembers;
        state->resourceSetImmutableGrants = resourceSetImmutableGrants;
        state->resourceSetImmutableBytes =
            std::move(resourceSetImmutableBytes);
        state->resourceSetAudioGrants = resourceSetAudioGrants;
        state->resourceSetAudioSources =
            std::move(resourceSetAudioSources);
        state->resourceSetMemberCount = resourceParameterCount;
        state->ownerUse = std::move(ownerUse);
        state->probeLiveness = std::move(probeLiveness);
        state->runtime = std::move(runtime);
        state->stageGate = std::make_shared<
            detail::ComponentActivationStageGateV1>();
        ComponentActivationAuthorityV1 authority{state};
        if (!authority.pending())
            return rejected(ComponentActivationAuthorizationCodeV1::RuntimeInvalid);
        return {
            std::optional<ComponentActivationAuthorityV1>{
                std::move(authority)},
            ComponentActivationAuthorizationCodeV1::Ready,
        };
    }
    catch (const std::bad_alloc&)
    {
        return rejected(ComponentActivationAuthorizationCodeV1::AllocationFailed);
    }
}
catch (const std::bad_alloc&)
{
    return rejected(
        ComponentActivationAuthorizationCodeV1::AllocationFailed);
}
catch (...)
{
    return rejected(
        ComponentActivationAuthorizationCodeV1::SnapshotInvalid);
}
}
