#include "runtime/component_bundle/ComponentBundleReplacementController.h"

#include "runtime/component_activation/ComponentActivationAudioSourceFreezer.h"
#include "runtime/component_activation/ComponentActivationManifestProfile.h"

#include "talas/expr/Expression.h"

#include <Security/Security.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

namespace talas::runtime::component_bundle
{
namespace
{
using Controller = ComponentBundleReplacementController;
using Diagnostic = ComponentBundleReplacementControllerDiagnostic;
using State = ComponentBundleReplacementControllerState;
using Operation = ComponentBundleReplacementControllerOperation;
namespace activation = component_activation;
namespace probe = component_probe;

constexpr auto controlPollInterval = std::chrono::milliseconds(1);
constexpr auto operationDeadline = std::chrono::seconds(12);

[[nodiscard]] bool allZero(std::span<const std::uint8_t> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](auto value) { return value == 0; });
}

template <std::size_t Size>
[[nodiscard]] bool randomNonZero(
    std::array<std::uint8_t, Size>& value) noexcept
{
    return SecRandomCopyBytes(kSecRandomDefault, value.size(), value.data())
            == errSecSuccess
        && !allZero(value);
}

[[nodiscard]] std::uint64_t nextActivationGeneration() noexcept
{
    static std::atomic<std::uint64_t> generation{1};
    const auto value = generation.fetch_add(1, std::memory_order_relaxed);
    return value == 0 || value == std::numeric_limits<std::uint64_t>::max()
        ? 0
        : value;
}

[[nodiscard]] const talas::project::ProjectNode* exactPreservedNode(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId) noexcept
{
    const auto node = std::ranges::find(
        document.graph.nodes,
        nodeId,
        &talas::project::ProjectNode::instanceId);
    return node == document.graph.nodes.end()
        || !node->preservedComponent.has_value()
        ? nullptr
        : std::addressof(*node);
}

[[nodiscard]] bool resolveInitialEventValue(
    void* user,
    std::string_view name,
    talas::expr::ExpressionValue& value) noexcept
{
    const auto* settings = static_cast<const RenderSettings*>(user);
    if (settings == nullptr)
        return false;
    const auto binding = std::ranges::find_if(
        settings->eventValues,
        [&](const EventValueBinding& candidate) {
            return candidate.scopeId == globalRuntimeScope
                && candidate.name == name;
        });
    if (binding == settings->eventValues.end())
        return false;
    value = {binding->value, binding->domain};
    return true;
}

[[nodiscard]] const talas::project::ProjectParameterExpression* exactParameter(
    const talas::project::ProjectNode& node,
    std::string_view parameterId) noexcept
{
    const auto parameter = std::ranges::find(
        node.parameters,
        parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    return parameter == node.parameters.end()
        ? nullptr
        : std::addressof(*parameter);
}

[[nodiscard]] bool copyParameterText(
    std::string_view value,
    std::array<
        char,
        activation::maximumComponentActivationParameterTextBytesV1>& target,
    std::uint32_t& byteCount,
    bool requireNonEmpty) noexcept
{
    if ((requireNonEmpty && value.empty()) || value.size() > target.size())
        return false;
    byteCount = static_cast<std::uint32_t>(value.size());
    std::copy(value.begin(), value.end(), target.begin());
    return true;
}

[[nodiscard]] bool prepareStaticParameters(
    const ComponentBundleNode& candidateNode,
    const talas::project::ProjectNode& projectNode,
    const RenderSettings& settings,
    talas::expr::ExpressionLanguageVersion language,
    std::vector<activation::ComponentActivationStaticParameterV1>& result)
{
    result.clear();
    result.reserve(candidateNode.parameters.size());
    for (const auto& descriptor : candidateNode.parameters)
    {
        if (descriptor.id.empty()
            || descriptor.id.size()
                > activation::maximumComponentActivationDescriptorIdBytesV1)
        {
            return false;
        }
        const auto* authored = exactParameter(projectNode, descriptor.id);
        if (authored == nullptr)
            return false;
        activation::ComponentActivationStaticParameterV1 parameter;
        parameter.idBytes = static_cast<std::uint32_t>(descriptor.id.size());
        std::copy(
            descriptor.id.begin(), descriptor.id.end(), parameter.id.begin());
        switch (descriptor.kind)
        {
            case talas::core::ParameterKind::Expression:
            {
                if (!authored->expression.has_value())
                    return false;
                const auto parsed = talas::expr::parse(
                    *authored->expression,
                    language,
                    settings.expressionDefinitions);
                if (!parsed.ok())
                    return false;
                talas::expr::EvaluationContext context;
                context.sampleRate = settings.sampleRate;
                context.tempoBpm = settings.tempoBpm;
                context.rampsStartAtFirstValue = settings.rampsStartAtFirstValue;
                context.rampsHoldUntriggered = settings.rampsHoldUntriggered;
                context.user = const_cast<RenderSettings*>(&settings);
                context.resolveEvent = resolveInitialEventValue;
                talas::expr::ExpressionValue value;
                if (!talas::expr::tryEvaluate(
                        *parsed.expression,
                        context,
                        {descriptor.domain},
                        value)
                    || !std::isfinite(value.value))
                {
                    return false;
                }
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Scalar);
                parameter.scalarValue = value.value;
                break;
            }
            case talas::core::ParameterKind::Choice:
            {
                if (!authored->choice.has_value())
                    return false;
                const auto choice = std::ranges::find(
                    descriptor.choices, *authored->choice);
                if (choice == descriptor.choices.end())
                    return false;
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Integer);
                parameter.integerValue = std::distance(
                    descriptor.choices.begin(), choice);
                break;
            }
            case talas::core::ParameterKind::Boolean:
                if (!authored->boolean.has_value())
                    return false;
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Integer);
                parameter.integerValue = *authored->boolean ? 1 : 0;
                break;
            case talas::core::ParameterKind::Text:
                if (!authored->text.has_value()
                    || !copyParameterText(
                        *authored->text,
                        parameter.text,
                        parameter.textBytes,
                        false))
                {
                    return false;
                }
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Text);
                break;
            case talas::core::ParameterKind::Resource:
                if (!authored->resource.has_value()
                    || !copyParameterText(
                        *authored->resource,
                        parameter.text,
                        parameter.textBytes,
                        true))
                {
                    return false;
                }
                parameter.kind = static_cast<std::uint32_t>(
                    activation::ComponentActivationStaticParameterKindV1::
                        Resource);
                break;
        }
        result.push_back(parameter);
    }
    return activation::validateComponentActivationStaticParametersV1(result)
        == activation::ComponentActivationProtocolDiagnosticV1::Valid;
}

[[nodiscard]] talas::core::ResourceSnapshotPtr exactResourceSnapshot(
    const RenderSettings& settings,
    std::string_view resourceId) noexcept
{
    talas::core::ResourceSnapshotPtr result;
    for (const auto& candidate : settings.resourceSnapshots)
    {
        if (candidate == nullptr || candidate->id != resourceId)
            continue;
        if (result != nullptr)
            return {};
        result = candidate;
    }
    return result;
}

struct PreparedActivationInputs
{
    std::vector<activation::ComponentActivationStaticParameterV1>
        staticParameters;
    std::vector<talas::core::ResourceSnapshotPtr> resourceSnapshots;
    std::vector<std::optional<activation::ComponentActivationFrozenAudioSourceV1>>
        frozenAudioSources;
    std::vector<activation::ComponentActivationResourceSetInputV1>
        resourceSetInputs;
    activation::ComponentActivationMacOSProfileV1 profile =
        activation::ComponentActivationMacOSProfileV1::StatelessAudio;
};

[[nodiscard]] std::optional<PreparedActivationInputs>
prepareActivationInputs(
    const ComponentBundleSnapshot& snapshot,
    const talas::project::ProjectNode& projectNode,
    const RenderSettings& settings,
    talas::expr::ExpressionLanguageVersion language,
    const std::function<bool()>& cancelled)
{
    if (snapshot.manifest.nodes.size() != 1)
        return std::nullopt;
    const auto& node = snapshot.manifest.nodes.front();
    PreparedActivationInputs result;
    if (!prepareStaticParameters(
            node,
            projectNode,
            settings,
            language,
            result.staticParameters))
    {
        return std::nullopt;
    }
    for (const auto& parameter : result.staticParameters)
    {
        if (parameter.kind != static_cast<std::uint32_t>(
                activation::ComponentActivationStaticParameterKindV1::
                    Resource))
        {
            continue;
        }
        const auto resourceId = std::string_view{
            parameter.text.data(), parameter.textBytes};
        auto resource = exactResourceSnapshot(settings, resourceId);
        if (resource == nullptr || resource->generation == 0)
            return std::nullopt;
        if (std::ranges::any_of(
                result.resourceSnapshots,
                [&](const auto& prior) {
                    return prior != nullptr && prior->id == resource->id;
                }))
        {
            return std::nullopt;
        }
        result.resourceSnapshots.push_back(std::move(resource));
    }
    if (result.resourceSnapshots.size()
        > activation::maximumComponentActivationResourceSetMembersV1)
    {
        return std::nullopt;
    }
    result.frozenAudioSources.resize(result.resourceSnapshots.size());
    std::size_t audioCount = 0;
    activation::ComponentActivationResourceSetBudgetV1 resourceBudget;
    for (std::size_t index = 0; index < result.resourceSnapshots.size(); ++index)
    {
        const auto& resource = result.resourceSnapshots[index];
        if (resource == nullptr || resource->id.empty()
            || !talas::project::isCanonicalProjectAssetContentHash(
                resource->contentHash))
        {
            return std::nullopt;
        }
        std::uint64_t sourceBytes = 0;
        std::uint64_t workingBytes = 0;
        bool audioStream = false;
        switch (resource->kind)
        {
            case talas::core::ResourceSnapshotKind::Table:
                if (resource->tableSamples.empty()
                    || !resource->tableRightSamples.empty()
                    || resource->tableSamples.size()
                        > activation::
                            maximumComponentActivationImmutableTableElementsV1
                    || !resource->binaryBytes.empty()
                    || !resource->audioBytes.empty()
                    || resource->channelCount != 0
                    || resource->sampleRate != 0.0
                    || resource->frameCount != 0
                    || !resource->encoding.empty()
                    || !std::ranges::all_of(
                        resource->tableSamples,
                        [](double value) { return std::isfinite(value); }))
                {
                    return std::nullopt;
                }
                sourceBytes = static_cast<std::uint64_t>(
                    resource->tableSamples.size() * sizeof(double));
                break;
            case talas::core::ResourceSnapshotKind::Binary:
                if (resource->binaryBytes.empty()
                    || resource->binaryBytes.size()
                        > activation::
                            maximumComponentActivationImmutableBinaryBytesV1
                    || !resource->tableSamples.empty()
                    || !resource->tableRightSamples.empty()
                    || !resource->audioBytes.empty()
                    || resource->channelCount != 0
                    || resource->sampleRate != 0.0
                    || resource->frameCount != 0
                    || !resource->encoding.empty())
                {
                    return std::nullopt;
                }
                sourceBytes = static_cast<std::uint64_t>(
                    resource->binaryBytes.size());
                break;
            case talas::core::ResourceSnapshotKind::Audio:
            {
                ++audioCount;
                auto frozen = activation::
                    ComponentActivationAudioSourceFreezerV1::freeze(
                        *resource, cancelled);
                if (!frozen.ok())
                    return std::nullopt;
                sourceBytes = frozen.source->sourceByteCount();
                workingBytes = activation::
                    maximumComponentActivationAudioStreamWorkingBytesV1;
                audioStream = true;
                result.frozenAudioSources[index] = std::move(*frozen.source);
                break;
            }
            default:
                return std::nullopt;
        }
        constexpr auto transportPage = activation::
            maximumComponentActivationResourceSetTransportPageBytesV1;
        if (sourceBytes == 0
            || sourceBytes > std::numeric_limits<std::uint64_t>::max()
                    - (transportPage - 1U))
        {
            return std::nullopt;
        }
        const auto transportBytes =
            ((sourceBytes + transportPage - 1U) / transportPage)
            * transportPage;
        if (activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
                resourceBudget,
                {
                    .sourceByteCount = sourceBytes,
                    .transportByteCount = transportBytes,
                    .privateMappedByteCount = transportBytes,
                    .workingByteCount = workingBytes,
                    .audioStream = audioStream,
                })
            != activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                Valid)
        {
            return std::nullopt;
        }
    }
    if (!result.resourceSnapshots.empty()
        && activation::validateComponentActivationResourceSetBudgetV1(
               resourceBudget)
            != activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                Valid)
    {
        return std::nullopt;
    }
    if (result.resourceSnapshots.empty())
    {
        result.profile = activation::ComponentActivationMacOSProfileV1::
            StatelessAudio;
    }
    else if (result.resourceSnapshots.size() == 1)
    {
        result.profile = audioCount == 0
            ? activation::ComponentActivationMacOSProfileV1::ImmutableResource
            : activation::ComponentActivationMacOSProfileV1::BoundedAudioStream;
    }
    else
    {
        result.profile = audioCount == 0
            ? activation::ComponentActivationMacOSProfileV1::
                MultipleImmutableOnlyResources
            : activation::ComponentActivationMacOSProfileV1::
                MultipleImmutableResources;
        result.resourceSetInputs.resize(result.resourceSnapshots.size());
        for (std::size_t index = 0;
             index < result.resourceSnapshots.size();
             ++index)
        {
            result.resourceSetInputs[index].snapshot =
                result.resourceSnapshots[index];
            if (result.frozenAudioSources[index].has_value())
            {
                result.resourceSetInputs[index].audioSource =
                    std::addressof(*result.frozenAudioSources[index]);
            }
        }
    }
    if (!activation::componentActivationManifestClaimsMatchRuntimeProfileV1(
            snapshot, result.profile))
        return std::nullopt;
    return result;
}

[[nodiscard]] bool waitForProbe(
    probe::ComponentBundleProbeMacOSCoordinator& coordinator,
    const std::function<bool()>& cancelled)
{
    const auto deadline = std::chrono::steady_clock::now()
        + operationDeadline;
    while (coordinator.operationPending()
        && std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled())
        {
            static_cast<void>(coordinator.cancel());
            return false;
        }
        std::this_thread::sleep_for(controlPollInterval);
    }
    return !coordinator.operationPending()
        && coordinator.state()
            == probe::ComponentBundleProbeMacOSCoordinatorStateV1::Finished;
}

[[nodiscard]] bool waitForActivationState(
    const activation::ComponentActivationMacOSHost& host,
    activation::ComponentActivationMacOSHostStateV1 expected,
    const std::function<bool()>& cancelled)
{
    const auto deadline = std::chrono::steady_clock::now()
        + operationDeadline;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled())
            return false;
        const auto state = host.state();
        if (state == expected)
            return true;
        if (state
                == activation::ComponentActivationMacOSHostStateV1::
                    Quarantined
            || state
                == activation::ComponentActivationMacOSHostStateV1::Closed)
        {
            return false;
        }
        std::this_thread::sleep_for(controlPollInterval);
    }
    return false;
}
}

struct ComponentBundleReplacementController::Impl final
    : std::enable_shared_from_this<Impl>
{
    explicit Impl(
        ComponentBundleReplacementControllerInputs preparedInputs,
        std::array<
            std::uint8_t,
            componentBundleReplacementSessionNonceBytes> sessionNonce)
        : inputs(std::move(preparedInputs)),
          confirmation(sessionNonce)
    {
    }

    ~Impl()
    {
        cancelAndJoin();
    }

    [[nodiscard]] bool current(
        std::uint64_t generation,
        const std::stop_token& stopToken) const noexcept
    {
        const auto liveGeneration = inputs.liveDocumentGeneration.lock();
        return generation != 0
            && generation == operationGeneration.load(std::memory_order_acquire)
            && liveGeneration != nullptr
            && liveGeneration->load(std::memory_order_acquire)
                == inputs.documentGeneration
            && !cancellationRequested.load(std::memory_order_acquire)
            && !stopToken.stop_requested();
    }

    [[nodiscard]] bool documentCurrent() const noexcept
    {
        const auto liveGeneration = inputs.liveDocumentGeneration.lock();
        return liveGeneration != nullptr
            && liveGeneration->load(std::memory_order_acquire)
                == inputs.documentGeneration;
    }

    void publish(
        std::uint64_t generation,
        State nextState,
        Diagnostic nextDiagnostic,
        std::string nextMessage,
        ProjectStageOutcome nextStageOutcome =
            ProjectStageOutcome::CompileFailed)
    {
        if (generation == 0
            || generation
                != operationGeneration.load(std::memory_order_acquire))
        {
            return;
        }
        {
            const std::scoped_lock lock(mutex);
            message = std::move(nextMessage);
            stageOutcome = nextStageOutcome;
        }
        diagnostic.store(nextDiagnostic, std::memory_order_relaxed);
        state.store(nextState, std::memory_order_release);
    }

    void publishCancelled(std::uint64_t generation) noexcept
    {
        try
        {
            publish(
                generation,
                State::Cancelled,
                Diagnostic::Cancelled,
                "Component replacement was cancelled");
        }
        catch (...)
        {
            if (generation != 0
                && generation
                    == operationGeneration.load(std::memory_order_acquire))
            {
                diagnostic.store(
                    Diagnostic::Cancelled, std::memory_order_relaxed);
                state.store(State::Cancelled, std::memory_order_release);
            }
        }
    }

    void publishFailureNoexcept(
        std::uint64_t generation,
        Diagnostic failure,
        const char* failureMessage) noexcept
    {
        try
        {
            publish(
                generation,
                State::Failed,
                failure,
                failureMessage);
        }
        catch (...)
        {
            if (generation != 0
                && generation
                    == operationGeneration.load(std::memory_order_acquire))
            {
                diagnostic.store(failure, std::memory_order_relaxed);
                state.store(State::Failed, std::memory_order_release);
            }
        }
    }

    void publishSuccessNoexcept(
        std::uint64_t generation,
        std::string nextMessage,
        ProjectStageOutcome nextStageOutcome) noexcept
    {
        if (generation == 0
            || generation
                != operationGeneration.load(std::memory_order_acquire))
        {
            return;
        }
        try
        {
            const std::scoped_lock lock(mutex);
            message.swap(nextMessage);
            stageOutcome = nextStageOutcome;
        }
        catch (...)
        {
            // The graph adoption is already irreversible. Terminal truth must
            // remain Succeeded even if the optional presentation update fails.
        }
        diagnostic.store(Diagnostic::Succeeded, std::memory_order_relaxed);
        state.store(State::Succeeded, std::memory_order_release);
    }

    void clearPendingNoexcept() noexcept
    {
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> probe;
        std::shared_ptr<PreparedIsolatedComponentBinding> binding;
        std::optional<ComponentBundleReplacementReceipt> receipt;
        try
        {
            const std::scoped_lock lock(mutex);
            probe = std::move(pendingProbe);
            binding = std::move(pendingBinding);
            receipt = std::move(pendingReceipt);
            pendingReceipt.reset();
        }
        catch (...)
        {
            return;
        }
        probe.reset();
        binding.reset();
        receipt.reset();
    }

    void clearPreviewNoexcept() noexcept
    {
        std::shared_ptr<const ComponentBundleReplacementPreview> stalePreview;
        std::uint64_t staleGeneration = 0;
        try
        {
            const std::scoped_lock lock(mutex);
            stalePreview = std::move(preview);
            staleGeneration = previewGeneration;
            previewGeneration = 0;
        }
        catch (...)
        {
            return;
        }
        if (staleGeneration != 0)
            static_cast<void>(confirmation.cancel(staleGeneration));
        stalePreview.reset();
    }

    void retainProbeNoexcept(
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> probe)
        noexcept
    {
        try
        {
            const std::scoped_lock lock(mutex);
            retainedProbe = std::move(probe);
        }
        catch (...)
        {
        }
    }

    void runInspectionSafely(
        std::uint64_t generation,
        std::filesystem::path selectedDirectory,
        std::stop_token stopToken) noexcept
    {
        try
        {
            runInspection(
                generation,
                std::move(selectedDirectory),
                stopToken);
        }
        catch (const std::bad_alloc&)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement inspection exhausted bounded storage");
        }
        catch (const std::length_error&)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement inspection exceeded bounded storage");
        }
        catch (...)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::InternalFailure,
                "Replacement inspection failed closed");
        }
    }

    [[nodiscard]] bool preparePreview(
        std::uint64_t generation,
        ComponentOwnerAuthorizationEvidenceV1 ownerEvidence,
        const std::stop_token& stopToken)
    {
        const auto cancelled = [this, generation, stopToken] {
            return !current(generation, stopToken);
        };
        ComponentTrustEvidence trust;
        trust.candidate = candidateFingerprint.view();
        trust.ownerAuthorizationEvidence = ownerEvidence;
        trust.signature = signatureEvidence;
        trust.declaredRoles = snapshotValue.manifest.declaredRoles;
        // The trust evaluator requires a nonzero prospective validation
        // session binding even while probe state is NotRun. This nonce grants
        // no probe authority and is replaced by the coordinator nonce.
        if (!randomNonZero(trust.validationNonce))
        {
            publish(
                generation,
                State::Failed,
                Diagnostic::InternalFailure,
                "Replacement preview validation identity is unavailable");
            return false;
        }
        trust.artifactGeneration = inputs.documentGeneration;
        trust.hasValidationNonce = true;
        const auto* node = exactPreservedNode(inputs.document, inputs.nodeId);
        if (node == nullptr)
        {
            publish(
                generation,
                State::Failed,
                Diagnostic::PreviewRejected,
                "The preserved replacement node is no longer available");
            return false;
        }
        // Compatibility is owner-visible only after the exact preserved
        // parameter values and immutable project resources satisfy the same
        // closed Step-8 preparation and aggregate budget used by activation.
        // The prepared values are intentionally discarded here and frozen
        // again after confirmation so a changed source still fails closed.
        if (!prepareActivationInputs(
                 snapshotValue,
                 *node,
                 inputs.renderSettings,
                 inputs.document.expressionLanguage,
                 cancelled)
                 .has_value())
        {
            if (cancelled())
                publishCancelled(generation);
            else
            {
                publish(
                    generation,
                    State::Failed,
                    Diagnostic::PreviewRejected,
                    "Replacement parameters or immutable resources do not satisfy the active Step-8 profile");
            }
            return false;
        }
        auto prepared = confirmation.prepare({
            inputs.documentGeneration,
            inputs.nodeId,
            {
                node->typeId,
                std::addressof(*node->preservedComponent),
                std::addressof(snapshotValue),
                std::addressof(trust),
                inputs.resourceTransitions,
            },
        });
        if (cancelled())
        {
            if (prepared.previewGeneration != 0)
            {
                static_cast<void>(
                    confirmation.cancel(prepared.previewGeneration));
            }
            publishCancelled(generation);
            return false;
        }
        if (!prepared.ready())
        {
            publish(
                generation,
                State::Failed,
                Diagnostic::PreviewRejected,
                prepared.preview != nullptr
                    ? prepared.preview->diagnostic
                    : "Replacement preview could not be prepared");
            return false;
        }
        {
            const std::scoped_lock lock(mutex);
            ownerAuthorizationEvidence = ownerEvidence;
            preview = std::move(prepared.preview);
            previewGeneration = prepared.previewGeneration;
            message = "Review the exact component replacement preview";
        }
        diagnostic.store(
            Diagnostic::AwaitingConfirmation, std::memory_order_relaxed);
        state.store(State::AwaitingConfirmation, std::memory_order_release);
        return true;
    }

    void preparePreviewSafely(
        std::uint64_t generation,
        ComponentOwnerAuthorizationEvidenceV1 ownerEvidence,
        std::stop_token stopToken) noexcept
    {
        try
        {
            static_cast<void>(preparePreview(
                generation, std::move(ownerEvidence), stopToken));
        }
        catch (const std::bad_alloc&)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement preview exhausted bounded storage");
        }
        catch (const std::length_error&)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement preview exceeded bounded storage");
        }
        catch (...)
        {
            clearPreviewNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::InternalFailure,
                "Replacement preview failed closed");
        }
    }

    void runInspection(
        std::uint64_t generation,
        std::filesystem::path selectedDirectory,
        std::stop_token stopToken)
    {
        const auto cancelled = [this, generation, stopToken] {
            return !current(generation, stopToken);
        };
        auto inspected = inspectComponentBundle(
            selectedDirectory, cancelled);
        if (cancelled())
        {
            publishCancelled(generation);
            return;
        }
        if (!inspected.ok())
        {
            publish(
                generation,
                State::Failed,
                Diagnostic::CandidateRejected,
                inspected.errors.empty()
                    ? "Selected component bundle could not be inspected"
                    : inspected.errors.front().summary);
            return;
        }
        auto candidateSnapshot = std::move(*inspected.snapshot);
        ComponentBundleSignatureVerificationResult verified;
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
        if (inputs.signatureVerifierForTesting)
        {
            verified = inputs.signatureVerifierForTesting(candidateSnapshot);
        }
        else
#endif
        {
            verified = verifyComponentBundleSignature(candidateSnapshot);
        }
        if (cancelled())
        {
            publishCancelled(generation);
            return;
        }
        if (!verified.ok())
        {
            publish(
                generation,
                State::Failed,
                Diagnostic::SignatureRejected,
                "Selected component bundle signature is not trusted");
            return;
        }
        const auto signature = verified.evidence.inspect();
        OwnedComponentApprovalFingerprint candidate;
        candidate.bundleId = candidateSnapshot.manifest.bundleId;
        candidate.semanticVersion = candidateSnapshot.manifest.semanticVersion;
        candidate.contentDigest = candidateSnapshot.contentDigest;
        candidate.signerIdentityDigest = signature.signerIdentityDigest;
        candidate.requestedPermissions =
            candidateSnapshot.manifest.requestedPermissions;
        candidate.policyVersion = currentComponentTrustPolicyVersion;
        candidate.hasContentDigest = true;
        candidate.hasSignerIdentity = true;
        {
            const std::scoped_lock lock(mutex);
            snapshotValue = std::move(candidateSnapshot);
            signatureEvidence = verified.evidence;
            candidateFingerprint = std::move(candidate);
            candidateProjection.bundleId =
                snapshotValue.manifest.bundleId;
            candidateProjection.semanticVersion =
                snapshotValue.manifest.semanticVersion;
            candidateProjection.publisher =
                snapshotValue.manifest.provenance.publisher;
            candidateProjection.licenseExpression =
                snapshotValue.manifest.provenance.licenseExpression;
            candidateProjection.contentDigest =
                snapshotValue.contentDigest;
            candidateProjection.signerIdentityDigest =
                signature.signerIdentityDigest;
            candidateProjection.requestedPermissions =
                snapshotValue.manifest.requestedPermissions;
            candidateProjection.available = true;
        }
        const auto ownerEvidence =
            inputs.ownerAuthorization->authorizationEvidence();
        const auto ownerMatch = ownerEvidence.inspect(
            candidateFingerprint.view());
        if (ownerMatch.ownerApproval
                != ComponentOwnerApprovalState::ExactMatch
            || ownerMatch.diagnostic
                != ComponentOwnerApprovalMatchCode::ExactMatch)
        {
            {
                const std::scoped_lock lock(mutex);
                message =
                    "Approve this exact bundle, content digest, and signer for the current session before compatibility inspection";
            }
            diagnostic.store(
                Diagnostic::OwnerApprovalRequired,
                std::memory_order_relaxed);
            state.store(
                State::AwaitingOwnerApproval,
                std::memory_order_release);
            return;
        }
        static_cast<void>(preparePreview(
            generation,
            ownerEvidence,
            stopToken));
    }

    void runConfirmed(
        std::uint64_t generation,
        ComponentBundleReplacementReceipt receipt,
        std::stop_token stopToken)
    {
        const auto cancelled = [this, generation, stopToken] {
            return !current(generation, stopToken);
        };
        auto fail = [&](Diagnostic code, std::string text) {
            if (cancelled())
                publishCancelled(generation);
            else
                publish(generation, State::Failed, code, std::move(text));
        };

        const auto* projectNode = exactPreservedNode(
            inputs.document, inputs.nodeId);
        if (projectNode == nullptr)
        {
            fail(
                Diagnostic::PreviewRejected,
                "The confirmed preserved node is no longer available");
            return;
        }
        auto activationInputs = prepareActivationInputs(
            snapshotValue,
            *projectNode,
            inputs.renderSettings,
            inputs.document.expressionLanguage,
            cancelled);
        if (!activationInputs.has_value())
        {
            fail(
                Diagnostic::ActivationRejected,
                "Replacement parameters or immutable resources could not be frozen exactly");
            return;
        }

        probe::ComponentBundleProbeMacOSCoordinatorInputsV1 probeInputs;
        probeInputs.snapshot = snapshotValue;
        probeInputs.signature = signatureEvidence;
        probeInputs.ownerAuthorization = ownerAuthorizationEvidence;
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
        const auto deadlineProfile = inputs.probeDeadlineProfileForTesting;
#else
        constexpr auto deadlineProfile =
            probe::ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
                Production;
#endif
        auto preparedProbe =
            probe::ComponentBundleProbeMacOSCoordinator::prepare(
                std::move(probeInputs), deadlineProfile);
        const auto probePrepared = preparedProbe.coordinator != nullptr
            && preparedProbe.diagnostic
                == probe::
                    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                        Ready;
        const auto probeStarted = probePrepared
            && preparedProbe.coordinator->start()
                == probe::ComponentBundleProbeMacOSCoordinatorOperationV1::
                    Scheduled;
        const auto probeFinished = probeStarted
            && waitForProbe(*preparedProbe.coordinator, cancelled);
        if (!probeFinished)
        {
            auto detail = std::string{
                "Disposable replacement probe did not produce clean live evidence"};
            if (preparedProbe.coordinator != nullptr)
            {
                detail += " (state="
                    + std::to_string(static_cast<unsigned>(
                        preparedProbe.coordinator->state()))
                    + ", diagnostic="
                    + std::to_string(static_cast<unsigned>(
                        preparedProbe.coordinator->diagnostic()))
                    + ", result="
                    + std::to_string(static_cast<unsigned>(
                        preparedProbe.coordinator->resultDiagnostic()))
                    + ")";
            }
            fail(
                Diagnostic::ProbeRejected,
                std::move(detail));
            return;
        }
        if (cancelled())
        {
            publishCancelled(generation);
            return;
        }
        state.store(State::Activating, std::memory_order_release);
        diagnostic.store(Diagnostic::Ready, std::memory_order_relaxed);

#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
        const auto endpoint = inputs.activationEndpointForTesting;
#else
        constexpr auto endpoint =
            activation::ComponentActivationMacOSEndpointV1::Production;
#endif
        auto preparedHost = activation::ComponentActivationMacOSHost::prepare(
            endpoint, activationInputs->profile);
        if (!preparedHost.ok()
            || preparedHost.host->start()
                != activation::ComponentActivationMacOSHostOperationV1::
                    Scheduled
            || !waitForActivationState(
                *preparedHost.host,
                activation::ComponentActivationMacOSHostStateV1::Ready,
                cancelled))
        {
            auto detail = std::string{
                "Replacement Runtime helper did not become ready"};
            if (preparedHost.host != nullptr)
            {
                detail += " (state="
                    + std::to_string(static_cast<unsigned>(
                        preparedHost.host->state()))
                    + ", diagnostic="
                    + std::to_string(static_cast<unsigned>(
                        preparedHost.host->diagnostic()))
                    + ")";
            }
            if (preparedHost.host != nullptr)
                preparedHost.host->shutdown();
            fail(
                Diagnostic::ActivationRejected,
                std::move(detail));
            return;
        }

        std::array<
            std::uint8_t,
            activation::componentActivationNonceBytesV1> authorizationNonce{};
        const auto activationGeneration = nextActivationGeneration();
        if (activationGeneration == 0 || !randomNonZero(authorizationNonce))
        {
            preparedHost.host->shutdown();
            fail(
                Diagnostic::InternalFailure,
                "Replacement activation session identity is unavailable");
            return;
        }
        activation::ComponentActivationAuthorizationInputsV1 authorization;
        authorization.snapshot = std::addressof(snapshotValue);
        authorization.signature = signatureEvidence;
        authorization.ownerAuthorization = ownerAuthorizationEvidence;
        authorization.probe = preparedProbe.coordinator->evidence();
        authorization.runtime = preparedHost.host->runtimeEvidence();
        authorization.validationNonce =
            preparedProbe.coordinator->validationNonce();
        authorization.authorizationNonce = authorizationNonce;
        authorization.descriptorId = projectNode->typeId;
        authorization.artifactGeneration =
            preparedProbe.coordinator->artifactGeneration();
        authorization.activationGeneration = activationGeneration;
        authorization.sampleRate = static_cast<std::uint32_t>(
            std::llround(inputs.renderSettings.sampleRate));
        authorization.blockFrames = static_cast<std::uint32_t>(
            inputs.renderSettings.maxBlockFrames);
        authorization.inputChannelCount = static_cast<std::uint16_t>(
            snapshotValue.manifest.nodes.front().inputs.size());
        authorization.outputChannelCount = static_cast<std::uint16_t>(
            snapshotValue.manifest.nodes.front().outputs.size());
        authorization.opaqueStateByteCount = static_cast<std::uint16_t>(
            projectNode->preservedComponent->state.size());
        authorization.staticParameters =
            activationInputs->staticParameters;
        if (activationInputs->resourceSnapshots.size() == 1)
        {
            authorization.resourceSnapshot =
                activationInputs->resourceSnapshots.front();
            if (activationInputs->frozenAudioSources.front().has_value())
            {
                authorization.audioSource = std::addressof(
                    *activationInputs->frozenAudioSources.front());
            }
        }
        else if (activationInputs->resourceSnapshots.size() > 1)
        {
            authorization.resourceSetInputs =
                activationInputs->resourceSetInputs;
        }
        auto authority =
            activation::ComponentActivationAuthorizationFactory::make(
                authorization);
        auto lease = authority.ok() ? authority.authority->consume()
                                    : std::nullopt;
        if (!lease.has_value()
            || preparedHost.host->activate(
                std::move(*lease),
                snapshotValue,
                activationInputs->staticParameters)
                != activation::ComponentActivationMacOSHostOperationV1::
                    Scheduled
            || !waitForActivationState(
                *preparedHost.host,
                activation::ComponentActivationMacOSHostStateV1::
                    PreparedActivation,
                cancelled))
        {
            const auto detail = std::string{
                "Exact replacement activation was rejected or revoked (state="}
                + std::to_string(static_cast<unsigned>(
                    preparedHost.host->state()))
                + ", diagnostic="
                + std::to_string(static_cast<unsigned>(
                    preparedHost.host->diagnostic()))
                + ")";
            preparedHost.host->shutdown();
            fail(
                Diagnostic::ActivationRejected,
                detail);
            return;
        }
        auto binding = std::move(*preparedHost.host)
            .takePreparedIsolatedComponentBinding(
                inputs.nodeId,
                snapshotValue,
                activationInputs->staticParameters);
        if (binding == nullptr || !binding->active())
        {
            fail(
                Diagnostic::ActivationRejected,
                "Replacement Runtime binding could not be retained");
            return;
        }
        if (cancelled())
        {
            publishCancelled(generation);
            return;
        }
        {
            const std::scoped_lock lock(mutex);
            pendingBinding = std::move(binding);
            pendingReceipt.emplace(std::move(receipt));
            pendingProbe = std::move(preparedProbe.coordinator);
            message = "Replacement binding is ready for control-thread staging";
        }
        diagnostic.store(Diagnostic::Ready, std::memory_order_relaxed);
        state.store(State::Staging, std::memory_order_release);
    }

    void runConfirmedSafely(
        std::uint64_t generation,
        ComponentBundleReplacementReceipt receipt,
        std::stop_token stopToken) noexcept
    {
        try
        {
            runConfirmed(generation, std::move(receipt), stopToken);
        }
        catch (const std::bad_alloc&)
        {
            clearPendingNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement orchestration exhausted bounded storage");
        }
        catch (const std::length_error&)
        {
            clearPendingNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::AllocationFailed,
                "Replacement orchestration exceeded bounded storage");
        }
        catch (...)
        {
            clearPendingNoexcept();
            publishFailureNoexcept(
                generation,
                Diagnostic::InternalFailure,
                "Replacement orchestration failed closed");
        }
    }

    void cancelAndJoin() noexcept
    {
        cancellationRequested.store(true, std::memory_order_release);
        if (worker.joinable())
        {
            worker.request_stop();
            worker.join();
        }
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> probe;
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator>
            pendingProbeValue;
        std::shared_ptr<PreparedIsolatedComponentBinding> pendingBindingValue;
        std::optional<ComponentBundleReplacementReceipt> pendingReceiptValue;
        {
            const std::scoped_lock lock(mutex);
            probe = std::move(retainedProbe);
            pendingProbeValue = std::move(pendingProbe);
            pendingBindingValue = std::move(pendingBinding);
            pendingReceiptValue = std::move(pendingReceipt);
            pendingReceipt.reset();
        }
        probe.reset();
        pendingProbeValue.reset();
        pendingBindingValue.reset();
        pendingReceiptValue.reset();
    }

    ComponentBundleReplacementControllerInputs inputs;
    ComponentBundleReplacementConfirmationStore confirmation;
    mutable std::mutex mutex;
    mutable std::mutex stageMutex;
    std::atomic<State> state{State::Idle};
    std::atomic<Diagnostic> diagnostic{Diagnostic::Ready};
    std::atomic<std::uint64_t> operationGeneration{0};
    std::atomic<bool> cancellationRequested{false};
    std::jthread worker;
    ComponentBundleSnapshot snapshotValue;
    ComponentSignatureEvidence signatureEvidence;
    OwnedComponentApprovalFingerprint candidateFingerprint;
    ComponentBundleReplacementCandidateProjection candidateProjection;
    ComponentOwnerAuthorizationEvidenceV1 ownerAuthorizationEvidence;
    std::shared_ptr<const ComponentBundleReplacementPreview> preview;
    std::uint64_t previewGeneration = 0;
    ProjectStageOutcome stageOutcome = ProjectStageOutcome::CompileFailed;
    std::string message;
    std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> retainedProbe;
    std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> pendingProbe;
    std::shared_ptr<PreparedIsolatedComponentBinding> pendingBinding;
    std::optional<ComponentBundleReplacementReceipt> pendingReceipt;
};

ComponentBundleReplacementController::ComponentBundleReplacementController(
    std::shared_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentBundleReplacementController::~ComponentBundleReplacementController()
{
    if (impl_ != nullptr)
        impl_->cancelAndJoin();
}

ComponentBundleReplacementControllerPreparation
ComponentBundleReplacementController::prepare(
    ComponentBundleReplacementControllerInputs inputs) noexcept
try
{
    ComponentBundleReplacementControllerPreparation result;
    const auto* node = exactPreservedNode(inputs.document, inputs.nodeId);
    const auto liveGeneration = inputs.liveDocumentGeneration.lock();
    if (inputs.bridge == nullptr || inputs.ownerAuthorization == nullptr
        || inputs.documentGeneration == 0 || node == nullptr
        || liveGeneration == nullptr
        || liveGeneration->load(std::memory_order_acquire)
            != inputs.documentGeneration
        || inputs.resourceTransitions.size()
            > maximumComponentBundleReplacementTransitions
        || !std::isfinite(inputs.renderSettings.sampleRate)
        || inputs.renderSettings.sampleRate < 1.0
        || inputs.renderSettings.sampleRate
            > std::numeric_limits<std::uint32_t>::max()
        || std::floor(inputs.renderSettings.sampleRate)
            != inputs.renderSettings.sampleRate
        || !activation::componentActivationSupportsSampleRateV1(
            static_cast<std::uint32_t>(inputs.renderSettings.sampleRate))
        || inputs.renderSettings.maxBlockFrames == 0
        || inputs.renderSettings.maxBlockFrames
            > std::numeric_limits<std::uint32_t>::max()
        || !activation::componentActivationSupportsBlockFramesV1(
            static_cast<std::uint32_t>(
                inputs.renderSettings.maxBlockFrames))
        || node->preservedComponent->state.size()
            > std::numeric_limits<std::uint16_t>::max())
    {
        return result;
    }
    std::array<
        std::uint8_t,
        componentBundleReplacementSessionNonceBytes> sessionNonce{};
    if (!randomNonZero(sessionNonce))
    {
        result.diagnostic = Diagnostic::InternalFailure;
        return result;
    }
    auto implementation = std::make_shared<Impl>(
        std::move(inputs), sessionNonce);
    result.controller = std::unique_ptr<Controller>(
        new Controller{std::move(implementation)});
    result.diagnostic = Diagnostic::Ready;
    return result;
}
catch (const std::bad_alloc&)
{
    return {
        nullptr,
        Diagnostic::AllocationFailed,
    };
}
catch (...)
{
    return {
        nullptr,
        Diagnostic::InternalFailure,
    };
}

Operation ComponentBundleReplacementController::inspect(
    std::filesystem::path selectedDirectory) noexcept
{
    if (impl_ == nullptr || selectedDirectory.empty()
        || impl_->state.load(std::memory_order_acquire) != State::Idle)
    {
        return Operation::InvalidState;
    }
    try
    {
        if (impl_->worker.joinable())
            impl_->worker.join();
        impl_->cancellationRequested.store(false, std::memory_order_release);
        auto generation = impl_->operationGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        if (generation == 0)
            return Operation::Rejected;
        impl_->diagnostic.store(Diagnostic::Inspecting, std::memory_order_relaxed);
        impl_->state.store(State::Inspecting, std::memory_order_release);
        const auto weak = std::weak_ptr<Impl>{impl_};
        impl_->worker = std::jthread{
            [weak,
             generation,
             selectedDirectory = std::move(selectedDirectory)](
                std::stop_token stopToken) mutable {
                if (const auto implementation = weak.lock())
                {
                    implementation->runInspectionSafely(
                        generation,
                        std::move(selectedDirectory),
                        stopToken);
                }
            }};
        return Operation::Scheduled;
    }
    catch (...)
    {
        const auto generation = impl_->operationGeneration.load(
            std::memory_order_acquire);
        impl_->publishFailureNoexcept(
            generation,
            Diagnostic::AllocationFailed,
            "Replacement inspection worker could not be started");
        return Operation::Rejected;
    }
}

Operation ComponentBundleReplacementController::approveCandidate() noexcept
try
{
    if (impl_ == nullptr
        || impl_->state.load(std::memory_order_acquire)
            != State::AwaitingOwnerApproval)
    {
        return Operation::InvalidState;
    }
    if (!impl_->documentCurrent())
    {
        static_cast<void>(cancel());
        return Operation::Rejected;
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    const auto generation = impl_->operationGeneration.load(
        std::memory_order_acquire);
    auto lookup = impl_->inputs.ownerAuthorization->lookup(
        impl_->candidateFingerprint.view());
    if (!lookup.approved())
    {
        const auto mutation = lookup.denied()
            ? impl_->inputs.ownerAuthorization->replaceOwnerAction(
                  impl_->candidateFingerprint.view(),
                  ComponentOwnerAuthorizationAction::Approve,
                  lookup.generation)
            : impl_->inputs.ownerAuthorization->recordOwnerAction(
                  impl_->candidateFingerprint.view(),
                  ComponentOwnerAuthorizationAction::Approve,
                  lookup.generation);
        if (!mutation.ok())
        {
            lookup = impl_->inputs.ownerAuthorization->lookup(
                impl_->candidateFingerprint.view());
            if (!lookup.approved())
            {
                impl_->publish(
                    generation,
                    State::Failed,
                    Diagnostic::OwnerApprovalRequired,
                    "Exact session-local owner approval could not be recorded");
                return Operation::Rejected;
            }
        }
    }
    const auto ownerEvidence =
        impl_->inputs.ownerAuthorization->authorizationEvidence();
    const auto ownerMatch = ownerEvidence.inspect(
        impl_->candidateFingerprint.view());
    if (ownerMatch.ownerApproval
            != ComponentOwnerApprovalState::ExactMatch
        || ownerMatch.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return Operation::Rejected;
    }
    impl_->cancellationRequested.store(false, std::memory_order_release);
    impl_->diagnostic.store(Diagnostic::Inspecting, std::memory_order_relaxed);
    impl_->state.store(State::Inspecting, std::memory_order_release);
    const auto weak = std::weak_ptr<Impl>{impl_};
    impl_->worker = std::jthread{
        [weak, generation, ownerEvidence](std::stop_token stopToken) mutable {
            if (const auto implementation = weak.lock())
            {
                implementation->preparePreviewSafely(
                    generation, std::move(ownerEvidence), stopToken);
            }
        }};
    return Operation::Scheduled;
}
catch (const std::bad_alloc&)
{
    if (impl_ != nullptr)
    {
        impl_->publishFailureNoexcept(
            impl_->operationGeneration.load(std::memory_order_acquire),
            Diagnostic::AllocationFailed,
            "Session-local owner approval could not be allocated");
    }
    return Operation::Rejected;
}
catch (...)
{
    if (impl_ != nullptr)
    {
        impl_->publishFailureNoexcept(
            impl_->operationGeneration.load(std::memory_order_acquire),
            Diagnostic::InternalFailure,
            "Session-local owner approval failed");
    }
    return Operation::Rejected;
}

Operation ComponentBundleReplacementController::confirm(
    std::uint64_t requestedPreviewGeneration) noexcept
{
    if (impl_ == nullptr || requestedPreviewGeneration == 0
        || impl_->state.load(std::memory_order_acquire)
            != State::AwaitingConfirmation)
    {
        return Operation::InvalidState;
    }
    if (!impl_->documentCurrent())
    {
        static_cast<void>(cancel());
        return Operation::Rejected;
    }
    try
    {
        if (impl_->worker.joinable())
            impl_->worker.join();
        std::uint64_t exactPreviewGeneration = 0;
        {
            const std::scoped_lock lock(impl_->mutex);
            exactPreviewGeneration = impl_->previewGeneration;
        }
        if (requestedPreviewGeneration != exactPreviewGeneration)
            return Operation::Rejected;
        auto receipt = impl_->confirmation.confirm(requestedPreviewGeneration);
        if (!receipt.has_value())
            return Operation::Rejected;
        impl_->cancellationRequested.store(false, std::memory_order_release);
        auto generation = impl_->operationGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        if (generation == 0)
            return Operation::Rejected;
        impl_->diagnostic.store(Diagnostic::Ready, std::memory_order_relaxed);
        impl_->state.store(State::Probing, std::memory_order_release);
        const auto weak = std::weak_ptr<Impl>{impl_};
        impl_->worker = std::jthread{
            [weak,
             generation,
             receipt = std::move(*receipt)](
                std::stop_token stopToken) mutable {
                if (const auto implementation = weak.lock())
                {
                    implementation->runConfirmedSafely(
                        generation, std::move(receipt), stopToken);
                }
            }};
        return Operation::Scheduled;
    }
    catch (...)
    {
        const auto generation = impl_->operationGeneration.load(
            std::memory_order_acquire);
        impl_->publishFailureNoexcept(
            generation,
            Diagnostic::AllocationFailed,
            "Replacement orchestration worker could not be started");
        return Operation::Rejected;
    }
}

Operation ComponentBundleReplacementController::stage() noexcept
{
    if (impl_ == nullptr)
    {
        return Operation::InvalidState;
    }
    auto generation = std::uint64_t{0};
    try
    {
        const std::scoped_lock stageLock(impl_->stageMutex);
        if (impl_->state.load(std::memory_order_acquire) != State::Staging
            || impl_->cancellationRequested.load(std::memory_order_acquire))
        {
            return Operation::InvalidState;
        }
        if (!impl_->documentCurrent())
        {
            impl_->cancellationRequested.store(true, std::memory_order_release);
            generation = impl_->operationGeneration.load(
                std::memory_order_acquire);
            impl_->clearPendingNoexcept();
            impl_->publishCancelled(generation);
            return Operation::Rejected;
        }
        generation = impl_->operationGeneration.load(std::memory_order_acquire);
        if (impl_->worker.joinable())
            impl_->worker.join();
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> probe;
        std::shared_ptr<PreparedIsolatedComponentBinding> binding;
        std::optional<ComponentBundleReplacementReceipt> receipt;
        {
            const std::scoped_lock lock(impl_->mutex);
            probe = std::move(impl_->pendingProbe);
            binding = std::move(impl_->pendingBinding);
            receipt = std::move(impl_->pendingReceipt);
            impl_->pendingReceipt.reset();
        }
        if (probe == nullptr || binding == nullptr || !receipt.has_value())
        {
            impl_->publish(
                generation,
                State::Failed,
                Diagnostic::InternalFailure,
                "Replacement staging context is incomplete");
            return Operation::Rejected;
        }
        std::string successMessage{"Component replacement was staged"};
        auto result = impl_->inputs.bridge
            ->compileAndStageIsolatedComponentReplacement(
                impl_->inputs.document,
                impl_->inputs.renderSettings,
                std::move(binding),
                impl_->inputs.documentGeneration,
                impl_->inputs.nodeId,
                impl_->candidateFingerprint.view(),
                std::move(*receipt));
        if (!result.ok())
        {
            impl_->publish(
                generation,
                State::Failed,
                Diagnostic::StageRejected,
                result.message.empty()
                    ? "Replacement graph stage was rejected"
                    : result.message,
                result.outcome);
            return Operation::Rejected;
        }
        if (!result.message.empty())
            successMessage = std::move(result.message);
        impl_->retainProbeNoexcept(std::move(probe));
        impl_->publishSuccessNoexcept(
            generation, std::move(successMessage), result.outcome);
        return Operation::Scheduled;
    }
    catch (const std::bad_alloc&)
    {
        impl_->publishFailureNoexcept(
            generation,
            Diagnostic::AllocationFailed,
            "Replacement staging exhausted bounded storage");
        return Operation::Rejected;
    }
    catch (const std::length_error&)
    {
        impl_->publishFailureNoexcept(
            generation,
            Diagnostic::AllocationFailed,
            "Replacement staging exceeded bounded storage");
        return Operation::Rejected;
    }
    catch (...)
    {
        impl_->publishFailureNoexcept(
            generation,
            Diagnostic::InternalFailure,
            "Replacement staging failed closed");
        return Operation::Rejected;
    }
}

Operation ComponentBundleReplacementController::cancel() noexcept
{
    if (impl_ == nullptr)
        return Operation::InvalidState;
    std::unique_lock<std::mutex> stageLock;
    try
    {
        stageLock = std::unique_lock<std::mutex>{impl_->stageMutex};
    }
    catch (...)
    {
        return Operation::Rejected;
    }
    const auto currentState = impl_->state.load(std::memory_order_acquire);
    if (currentState == State::Idle || currentState == State::Cancelled
        || currentState == State::Failed)
        return Operation::InvalidState;
    const auto generation = impl_->operationGeneration.load(
        std::memory_order_acquire);
    impl_->cancellationRequested.store(true, std::memory_order_release);
    if (impl_->worker.joinable())
        impl_->worker.request_stop();
    if (currentState == State::Inspecting
        || currentState == State::Probing
        || currentState == State::Activating)
    {
        if (impl_->worker.joinable())
            impl_->worker.join();
        if (impl_->state.load(std::memory_order_acquire) == State::Staging)
        {
            std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator>
                pendingProbe;
            std::shared_ptr<PreparedIsolatedComponentBinding> pendingBinding;
            std::optional<ComponentBundleReplacementReceipt> pendingReceipt;
            {
                const std::scoped_lock lock(impl_->mutex);
                pendingProbe = std::move(impl_->pendingProbe);
                pendingBinding = std::move(impl_->pendingBinding);
                pendingReceipt = std::move(impl_->pendingReceipt);
                impl_->pendingReceipt.reset();
            }
            pendingProbe.reset();
            pendingBinding.reset();
            pendingReceipt.reset();
        }
        impl_->clearPreviewNoexcept();
        impl_->clearPendingNoexcept();
        impl_->publishCancelled(generation);
        return Operation::Scheduled;
    }
    if (currentState == State::AwaitingOwnerApproval
        || currentState == State::AwaitingConfirmation)
    {
        std::uint64_t preparedPreview = 0;
        {
            const std::scoped_lock lock(impl_->mutex);
            preparedPreview = impl_->previewGeneration;
        }
        if (preparedPreview != 0)
            static_cast<void>(impl_->confirmation.cancel(preparedPreview));
        impl_->publishCancelled(generation);
    }
    else if (currentState == State::Succeeded)
    {
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator> retained;
        {
            const std::scoped_lock lock(impl_->mutex);
            retained = std::move(impl_->retainedProbe);
        }
        retained.reset();
        impl_->publishCancelled(generation);
    }
    else if (currentState == State::Staging)
    {
        if (impl_->worker.joinable())
            impl_->worker.join();
        std::unique_ptr<probe::ComponentBundleProbeMacOSCoordinator>
            pendingProbe;
        std::shared_ptr<PreparedIsolatedComponentBinding> pendingBinding;
        std::optional<ComponentBundleReplacementReceipt> pendingReceipt;
        {
            const std::scoped_lock lock(impl_->mutex);
            pendingProbe = std::move(impl_->pendingProbe);
            pendingBinding = std::move(impl_->pendingBinding);
            pendingReceipt = std::move(impl_->pendingReceipt);
            impl_->pendingReceipt.reset();
        }
        pendingProbe.reset();
        pendingBinding.reset();
        pendingReceipt.reset();
        impl_->publishCancelled(generation);
    }
    return Operation::Scheduled;
}

ComponentBundleReplacementControllerSnapshot
ComponentBundleReplacementController::snapshot() const
{
    ComponentBundleReplacementControllerSnapshot result;
    if (impl_ == nullptr)
    {
        result.state = State::Failed;
        result.diagnostic = Diagnostic::InvalidConfiguration;
        result.message = "Replacement controller is unavailable";
        return result;
    }
    result.operationGeneration = impl_->operationGeneration.load(
        std::memory_order_acquire);
    result.state = impl_->state.load(std::memory_order_acquire);
    result.diagnostic = impl_->diagnostic.load(std::memory_order_relaxed);
    {
        const std::scoped_lock lock(impl_->mutex);
        result.previewGeneration = impl_->previewGeneration;
        result.candidate = impl_->candidateProjection;
        result.preview = impl_->preview;
        result.stageOutcome = impl_->stageOutcome;
        result.message = impl_->message;
    }
    return result;
}
}
