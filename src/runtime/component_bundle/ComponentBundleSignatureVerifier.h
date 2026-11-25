#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"

#include "talas/runtime/ComponentTrustPolicy.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace talas::runtime::component_bundle
{
inline constexpr std::string_view componentBundleSignatureDomain =
    "talas.component-bundle.signature.v1";
inline constexpr std::string_view componentBundleSignerIdentityDomain =
    "talas.component-signer.macos-cms.v1";

enum class ComponentBundleSignatureVerificationCode : std::uint8_t
{
    Verified,
    MissingEnvelope,
    MalformedEnvelope,
    MultipleSigners,
    AttachedContentDenied,
    InvalidSignature,
    SnapshotIdentityMismatch,
    UntrustedSigner,
    ExpiredSigner,
    RevokedSigner,
    UnsupportedPlatform,
    VerifierFailure,
};

class ComponentBundleSignatureEvidenceFactory
{
public:
    [[nodiscard]] static constexpr ComponentSignatureEvidence make(
        ComponentSignatureEvidenceView view) noexcept
    {
        return ComponentSignatureEvidence{std::move(view)};
    }
};

struct ComponentBundleSignatureVerificationResult
{
    ComponentSignatureEvidence evidence;
    ComponentBundleSignatureVerificationCode code =
        ComponentBundleSignatureVerificationCode::VerifierFailure;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ComponentBundleSignatureVerificationCode::Verified
            && evidence.inspect().state == ComponentSignatureState::Verified;
    }
};

[[nodiscard]] std::vector<std::uint8_t>
makeComponentBundleSignatureMessage(
    const std::array<std::uint8_t, componentTrustDigestBytes>& contentDigest);

[[nodiscard]] ComponentBundleSignatureVerificationResult
verifyComponentBundleSignature(const ComponentBundleSnapshot& snapshot);

namespace detail
{
// Hermetic native tests use an explicit DER anchor. Product code must call the
// production verifier above, which evaluates only the system trust store.
[[nodiscard]] ComponentBundleSignatureVerificationResult
verifyComponentBundleSignatureWithAnchorForTesting(
    const ComponentBundleSnapshot& snapshot,
    std::span<const std::uint8_t> trustedAnchorDer);

[[nodiscard]] ComponentBundleSignatureVerificationResult
verifyComponentBundleSignatureAtUnixTimeForTesting(
    const ComponentBundleSnapshot& snapshot,
    std::span<const std::uint8_t> trustedAnchorDer,
    double unixTime);
}
}
