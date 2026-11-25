#include "runtime/component_bundle/ComponentBundleReplacementPolicy.h"

#include "talas/project/ProjectAssetInspection.h"
#include "runtime/component_activation/ComponentActivationManifestProfile.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>

namespace talas::runtime::component_bundle
{
namespace
{
constexpr std::string_view digestDomain =
    "talas.component-replacement.preserved-capsule.v1";

template <typename Integer>
void appendInteger(std::vector<std::uint8_t>& bytes, Integer value)
{
    using Unsigned = std::make_unsigned_t<Integer>;
    auto unsignedValue = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index)
    {
        bytes.push_back(static_cast<std::uint8_t>(
            unsignedValue >> (index * 8U)));
    }
}

void appendText(
    std::vector<std::uint8_t>& bytes,
    std::string_view value)
{
    appendInteger(
        bytes,
        static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

template <typename Values, typename Append>
void appendSequence(
    std::vector<std::uint8_t>& bytes,
    const Values& values,
    Append append)
{
    appendInteger(
        bytes,
        static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values)
        append(bytes, value);
}

[[nodiscard]] std::uint8_t hexadecimalNibble(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    return 0xffU;
}

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
decodeDigest(std::string_view hexadecimal) noexcept
{
    std::array<std::uint8_t, componentTrustDigestBytes> digest{};
    if (hexadecimal.size() != digest.size() * 2U)
        return {};
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        const auto high = hexadecimalNibble(hexadecimal[index * 2U]);
        const auto low = hexadecimalNibble(hexadecimal[index * 2U + 1U]);
        if (high > 0xfU || low > 0xfU)
            return {};
        digest[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return digest;
}

[[nodiscard]] bool candidateMatchesSnapshot(
    const ComponentApprovalFingerprint& candidate,
    const ComponentBundleSnapshot& snapshot) noexcept
{
    return candidate.bundleId == snapshot.manifest.bundleId
        && candidate.semanticVersion == snapshot.manifest.semanticVersion
        && candidate.contentDigest == snapshot.contentDigest
        && candidate.requestedPermissions
            == snapshot.manifest.requestedPermissions
        && candidate.policyVersion == currentComponentTrustPolicyVersion
        && candidate.hasContentDigest && candidate.hasSignerIdentity;
}

[[nodiscard]] bool portsMatch(
    const std::vector<talas::project::ProjectPreservedPort>& preserved,
    const std::vector<ComponentBundlePort>& candidate) noexcept
{
    if (preserved.size() != candidate.size())
        return false;
    for (std::size_t index = 0; index < preserved.size(); ++index)
    {
        const auto& expected = preserved[index];
        const auto& actual = candidate[index];
        if (expected.portId != actual.id
            || expected.type != actual.type
            || expected.required != actual.required
            || expected.signal.rate != actual.signal.rate
            || expected.signal.audioLayout != actual.signal.audioLayout
            || expected.signal.edge != actual.signal.edge
            || expected.signal.spectralFormat
                != actual.signal.spectralFormat
            || expected.signal.timelineBearing
                != actual.signal.timelineBearing)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parametersMatch(
    const std::vector<talas::project::ProjectPreservedParameter>& preserved,
    const std::vector<ComponentBundleParameter>& candidate) noexcept
{
    if (preserved.size() != candidate.size())
        return false;
    for (std::size_t index = 0; index < preserved.size(); ++index)
    {
        const auto& expected = preserved[index];
        const auto& actual = candidate[index];
        if (expected.parameterId != actual.id
            || expected.kind != actual.kind
            || expected.domain != actual.domain
            || expected.choices != actual.choices)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool canonicalHash(std::string_view value) noexcept
{
    return value.size() == componentTrustDigestBytes * 2U
        && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool validResourceTransitions(
    std::span<const ComponentBundleReplacementResourceTransition>
        transitions) noexcept
{
    if (transitions.size()
        > maximumComponentBundleReplacementTransitions)
    {
        return false;
    }
    for (std::size_t index = 0; index < transitions.size(); ++index)
    {
        const auto& transition = transitions[index];
        if (transition.previous.resourceId.empty()
            || transition.previous.resourceId
                != transition.candidate.resourceId
            || transition.previous.generation == 0
            || transition.candidate.generation == 0
            || !canonicalHash(transition.previous.contentHash)
            || !canonicalHash(transition.candidate.contentHash)
            || transition.candidate.generation
                < transition.previous.generation
            || (transition.candidate.generation
                    == transition.previous.generation
                && (transition.candidate.kind
                        != transition.previous.kind
                    || transition.candidate.contentHash
                        != transition.previous.contentHash))
            || (transition.candidate.generation
                    > transition.previous.generation
                && transition.candidate.contentHash
                    == transition.previous.contentHash))
        {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other)
        {
            if (transitions[other].previous.resourceId
                == transition.previous.resourceId)
            {
                return false;
            }
        }
        const auto audioCrossKind =
            transition.previous.kind != transition.candidate.kind
            && (transition.previous.kind
                    == talas::core::ResourceSnapshotKind::Audio
                || transition.candidate.kind
                    == talas::core::ResourceSnapshotKind::Audio);
        if (transition.requiresExplicitConfirmation != audioCrossKind)
            return false;
    }
    return true;
}

void setFailure(
    ComponentBundleReplacementPreview& preview,
    ComponentBundleReplacementCode code,
    std::string diagnostic)
{
    preview.code = code;
    preview.diagnostic = std::move(diagnostic);
}
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateProjectPreservedComponentDigest(
    std::string_view nodeTypeId,
    const talas::project::ProjectPreservedComponent& preserved)
{
    std::vector<std::uint8_t> bytes;
    appendText(bytes, digestDomain);
    appendText(bytes, nodeTypeId);
    appendInteger(bytes, preserved.schemaVersion);
    appendText(bytes, preserved.typeVersion);
    appendInteger(bytes, preserved.stateSchemaVersion);
    appendInteger(bytes, static_cast<std::uint64_t>(preserved.state.size()));
    bytes.insert(bytes.end(), preserved.state.begin(), preserved.state.end());
    appendText(bytes, preserved.displayName);
    appendText(bytes, preserved.bundleId);
    const auto appendPort = [](
                                std::vector<std::uint8_t>& output,
                                const talas::project::ProjectPreservedPort& port) {
        appendText(output, port.portId);
        appendInteger(output, static_cast<std::uint32_t>(port.type));
        appendInteger(output, static_cast<std::uint8_t>(port.required));
        appendInteger(output, static_cast<std::uint32_t>(port.signal.rate));
        appendInteger(
            output,
            static_cast<std::uint32_t>(port.signal.audioLayout));
        appendInteger(output, static_cast<std::uint32_t>(port.signal.edge));
        appendInteger(
            output,
            static_cast<std::uint32_t>(port.signal.spectralFormat));
        appendInteger(
            output,
            static_cast<std::uint8_t>(port.signal.timelineBearing));
    };
    appendSequence(bytes, preserved.inputs, appendPort);
    appendSequence(bytes, preserved.outputs, appendPort);
    appendSequence(
        bytes,
        preserved.parameters,
        [](std::vector<std::uint8_t>& output,
           const talas::project::ProjectPreservedParameter& parameter) {
            appendText(output, parameter.parameterId);
            appendInteger(
                output,
                static_cast<std::uint32_t>(parameter.kind));
            appendInteger(
                output,
                static_cast<std::uint32_t>(parameter.domain));
            appendSequence(
                output,
                parameter.choices,
                [](std::vector<std::uint8_t>& choiceOutput,
                   const std::string& choice) {
                    appendText(choiceOutput, choice);
                });
        });
    appendText(bytes, preserved.recoveryHint);
    return decodeDigest(talas::project::calculateProjectAssetSha256(bytes));
}

ComponentBundleReplacementPreview previewComponentBundleReplacement(
    const ComponentBundleReplacementRequest& request) noexcept
{
    ComponentBundleReplacementPreview preview;
    try
    {
        preview.nodeTypeId = request.nodeTypeId;
        if (request.preserved != nullptr)
        {
            preview.preservedBundleId = request.preserved->bundleId;
            preview.preservedVersion = request.preserved->typeVersion;
        }
        if (request.candidate != nullptr)
        {
            preview.candidateBundleId =
                request.candidate->manifest.bundleId;
            preview.candidateVersion =
                request.candidate->manifest.semanticVersion;
            preview.candidateContentDigest =
                request.candidate->contentDigest;
            preview.declaredRoles =
                request.candidate->manifest.declaredRoles;
            preview.requestedPermissions =
                request.candidate->manifest.requestedPermissions;
            preview.publisher =
                request.candidate->manifest.provenance.publisher;
            preview.licenseExpression =
                request.candidate->manifest.provenance.licenseExpression;
        }
        if (request.preserved == nullptr)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::InvalidPreservedCapsule,
                "Preserved component capsule is unavailable");
            return preview;
        }
        if (talas::project::validateProjectPreservedComponent(
                *request.preserved)
            || request.nodeTypeId.empty()
            || request.preserved->bundleId.empty())
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::InvalidPreservedCapsule,
                "Preserved component capsule is invalid");
            return preview;
        }
        preview.preservedCapsuleDigest =
            calculateProjectPreservedComponentDigest(
                request.nodeTypeId,
                *request.preserved);
        if (request.candidate == nullptr)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::MissingCandidate,
                "No inspected replacement candidate is available");
            return preview;
        }
        if (!validateComponentBundleSnapshotIdentity(*request.candidate))
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::InvalidSnapshot,
                "Replacement candidate snapshot is invalid");
            return preview;
        }
        if (request.trustEvidence == nullptr)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::TrustEvidenceMissing,
                "Replacement candidate trust evidence is unavailable");
            return preview;
        }
        if (!candidateMatchesSnapshot(
                request.trustEvidence->candidate,
                *request.candidate))
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::CandidateFingerprintMismatch,
                "Trust evidence belongs to a different candidate");
            return preview;
        }
        preview.candidateSignerIdentityDigest =
            request.trustEvidence->candidate.signerIdentityDigest;
        const auto trust = evaluateComponentTrust(*request.trustEvidence);
        if (trust.disposition != ComponentTrustDisposition::EligibleForProbe
            && trust.disposition
                != ComponentTrustDisposition::EligibleForIsolatedActivation)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::CandidateRejected,
                std::string(projectComponentTrust(trust).summary));
            return preview;
        }
        if (request.candidate->manifest.bundleId
            != request.preserved->bundleId)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::BundleMismatch,
                "Replacement bundle ID does not match the preserved component");
            return preview;
        }
        const auto node = std::ranges::find(
            request.candidate->manifest.nodes,
            request.nodeTypeId,
            &ComponentBundleNode::typeId);
        if (node == request.candidate->manifest.nodes.end())
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::NodeMissing,
                "Replacement bundle does not declare the preserved node type");
            return preview;
        }
        preview.stateSchemaVersion = node->stateSchemaVersion;
        preview.inputs = node->inputs;
        preview.outputs = node->outputs;
        preview.parameters = node->parameters;
        if (node->stateSchemaVersion
            != request.preserved->stateSchemaVersion)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::StateSchemaMismatch,
                "Replacement state schema requires a migration that is not enabled");
            return preview;
        }
        if (!portsMatch(request.preserved->inputs, node->inputs)
            || !portsMatch(request.preserved->outputs, node->outputs)
            || !parametersMatch(
                request.preserved->parameters,
                node->parameters))
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::InterfaceMismatch,
                "Replacement port or parameter contract is incompatible");
            return preview;
        }
        const auto interfaceFitsProtocol =
            request.preserved->inputs.size()
                    <= std::numeric_limits<std::uint16_t>::max()
            && request.preserved->outputs.size()
                    <= std::numeric_limits<std::uint16_t>::max()
            && request.preserved->state.size()
                    <= std::numeric_limits<std::uint16_t>::max();
        const auto activationProfile = interfaceFitsProtocol
            ? component_activation::
                supportedComponentActivationManifestProfileV1(
                    *request.candidate,
                    static_cast<std::uint16_t>(
                        request.preserved->inputs.size()),
                    static_cast<std::uint16_t>(
                        request.preserved->outputs.size()),
                    static_cast<std::uint16_t>(
                        request.preserved->state.size()))
            : component_activation::
                ComponentActivationManifestProfileV1::Unsupported;
        if (activationProfile
            == component_activation::
                ComponentActivationManifestProfileV1::Unsupported)
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::UnsupportedManifestSurface,
                "Replacement requires a component surface outside the closed Step 9 profile");
            return preview;
        }
        if (!validResourceTransitions(request.resourceTransitions))
        {
            setFailure(
                preview,
                ComponentBundleReplacementCode::ResourceTransitionInvalid,
                "Replacement resource transition set is malformed or stale");
            return preview;
        }
        preview.resourceTransitions.assign(
            request.resourceTransitions.begin(),
            request.resourceTransitions.end());
        preview.versionChanged =
            preview.preservedVersion != preview.candidateVersion;
        preview.code = ComponentBundleReplacementCode::Compatible;
        preview.diagnostic = preview.versionChanged
            ? "Exact compatible candidate requires owner confirmation"
            : "Exact changed candidate requires owner confirmation";
        return preview;
    }
    catch (const std::bad_alloc&)
    {
        preview.code = ComponentBundleReplacementCode::AllocationFailed;
        preview.diagnostic.clear();
        return preview;
    }
    catch (const std::length_error&)
    {
        preview.code = ComponentBundleReplacementCode::AllocationFailed;
        preview.diagnostic.clear();
        return preview;
    }
}
}
