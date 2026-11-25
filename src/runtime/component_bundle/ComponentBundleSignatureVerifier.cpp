#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"

#include "talas/project/ProjectAssetInspection.h"

#include <limits>
#include <optional>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/CMSDecoder.h>
#include <Security/SecCertificate.h>
#include <Security/SecPolicy.h>
#include <Security/SecTrust.h>
#endif

namespace talas::runtime::component_bundle
{
namespace
{
[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
decodeDigest(std::string_view value)
{
    std::array<std::uint8_t, componentTrustDigestBytes> digest{};
    if (value.size() != digest.size() * 2U)
        return {};
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        const auto high = nibble(value[index * 2U]);
        const auto low = nibble(value[index * 2U + 1U]);
        if (high < 0 || low < 0)
            return {};
        digest[index] = static_cast<std::uint8_t>(
            (high << 4U) | low);
    }
    return digest;
}

[[nodiscard]] ComponentBundleSignatureVerificationResult failed(
    ComponentSignatureState state,
    ComponentBundleSignatureVerificationCode code)
{
    ComponentBundleSignatureVerificationResult result;
    ComponentSignatureEvidenceView evidence;
    evidence.state = state;
    result.evidence = ComponentBundleSignatureEvidenceFactory::make(evidence);
    result.code = code;
    return result;
}

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
signerIdentityDigest(std::span<const std::uint8_t> certificateDer)
{
    std::vector<std::uint8_t> transcript(
        componentBundleSignerIdentityDomain.begin(),
        componentBundleSignerIdentityDomain.end());
    transcript.reserve(
        transcript.size() + 1U + sizeof(std::uint32_t)
        + certificateDer.size());
    transcript.push_back(0);
    const auto size = static_cast<std::uint32_t>(certificateDer.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        transcript.push_back(static_cast<std::uint8_t>(size >> shift));
    transcript.insert(
        transcript.end(),
        certificateDer.begin(),
        certificateDer.end());
    return decodeDigest(talas::project::calculateProjectAssetSha256(transcript));
}

#if defined(__APPLE__)
template <typename Type>
class UniqueCF
{
public:
    explicit UniqueCF(Type value = nullptr) noexcept : value_(value) {}
    ~UniqueCF()
    {
        if (value_ != nullptr)
            CFRelease(value_);
    }
    UniqueCF(const UniqueCF&) = delete;
    UniqueCF& operator=(const UniqueCF&) = delete;
    UniqueCF(UniqueCF&& other) noexcept : value_(other.value_)
    {
        other.value_ = nullptr;
    }
    UniqueCF& operator=(UniqueCF&& other) noexcept
    {
        if (this != &other)
        {
            if (value_ != nullptr)
                CFRelease(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] Type get() const noexcept { return value_; }
    [[nodiscard]] Type* out() noexcept { return &value_; }

private:
    Type value_ = nullptr;
};

[[nodiscard]] ComponentBundleSignatureVerificationResult verifyMacOS(
    const ComponentBundleSnapshot& snapshot,
    std::span<const std::uint8_t> trustedAnchorDer,
    std::optional<double> verificationUnixTime)
{
    if (!validateComponentBundleSnapshotIdentity(snapshot))
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::SnapshotIdentityMismatch);
    }
    if (!snapshot.detachedSignatureCms)
    {
        return failed(
            ComponentSignatureState::Missing,
            ComponentBundleSignatureVerificationCode::MissingEnvelope);
    }
    if (snapshot.detachedSignatureCms->empty()
        || snapshot.detachedSignatureCms->size()
            > maximumComponentBundleSignatureBytes)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }
    if (trustedAnchorDer.size() > maximumComponentBundleSignatureBytes)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }

    UniqueCF<CMSDecoderRef> decoder;
    if (CMSDecoderCreate(decoder.out()) != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }
    const auto signedMessage =
        makeComponentBundleSignatureMessage(snapshot.contentDigest);
    UniqueCF<CFDataRef> detached(CFDataCreate(
        kCFAllocatorDefault,
        signedMessage.data(),
        static_cast<CFIndex>(signedMessage.size())));
    if (detached.get() == nullptr
        || CMSDecoderSetDetachedContent(decoder.get(), detached.get())
            != errSecSuccess
        || CMSDecoderUpdateMessage(
               decoder.get(),
               snapshot.detachedSignatureCms->data(),
               snapshot.detachedSignatureCms->size()) != errSecSuccess
        || CMSDecoderFinalizeMessage(decoder.get()) != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }

    Boolean isEncrypted = false;
    if (CMSDecoderIsContentEncrypted(decoder.get(), &isEncrypted)
            != errSecSuccess
        || isEncrypted)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }

    size_t signerCount = 0;
    if (CMSDecoderGetNumSigners(decoder.get(), &signerCount)
        != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }
    if (signerCount != 1U)
    {
        return failed(
            ComponentSignatureState::Invalid,
            signerCount > 1U
                ? ComponentBundleSignatureVerificationCode::MultipleSigners
                : ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }

    UniqueCF<CFDataRef> embeddedContent;
    if (CMSDecoderCopyContent(decoder.get(), embeddedContent.out())
        != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }
    if (embeddedContent.get() != nullptr)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::AttachedContentDenied);
    }

    UniqueCF<SecPolicyRef> policy(SecPolicyCreateBasicX509());
    UniqueCF<SecTrustRef> trust;
    CMSSignerStatus signerStatus = kCMSSignerUnsigned;
    if (policy.get() == nullptr
        || CMSDecoderCopySignerStatus(
               decoder.get(),
               0,
               policy.get(),
               false,
               &signerStatus,
               trust.out(),
               nullptr) != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }
    if (signerStatus != kCMSSignerValid)
    {
        return failed(
            ComponentSignatureState::Invalid,
            signerStatus == kCMSSignerInvalidSignature
                    || signerStatus == kCMSSignerNeedsDetachedContent
                ? ComponentBundleSignatureVerificationCode::InvalidSignature
                : ComponentBundleSignatureVerificationCode::MalformedEnvelope);
    }
    if (trust.get() == nullptr
        || SecTrustSetNetworkFetchAllowed(trust.get(), false)
            != errSecSuccess)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }

    UniqueCF<CFDateRef> verificationDate;
    if (verificationUnixTime)
    {
        verificationDate = UniqueCF<CFDateRef>(CFDateCreate(
            kCFAllocatorDefault,
            *verificationUnixTime - kCFAbsoluteTimeIntervalSince1970));
        if (verificationDate.get() == nullptr
            || SecTrustSetVerifyDate(trust.get(), verificationDate.get())
                != errSecSuccess)
        {
            return failed(
                ComponentSignatureState::Invalid,
                ComponentBundleSignatureVerificationCode::VerifierFailure);
        }
    }

    UniqueCF<CFDataRef> anchorData;
    UniqueCF<SecCertificateRef> anchor;
    UniqueCF<CFArrayRef> anchors;
    if (!trustedAnchorDer.empty())
    {
        anchorData = UniqueCF<CFDataRef>(CFDataCreate(
            kCFAllocatorDefault,
            trustedAnchorDer.data(),
            static_cast<CFIndex>(trustedAnchorDer.size())));
        if (anchorData.get() != nullptr)
        {
            anchor = UniqueCF<SecCertificateRef>(
                SecCertificateCreateWithData(
                    kCFAllocatorDefault,
                    anchorData.get()));
        }
        if (anchor.get() == nullptr)
        {
            return failed(
                ComponentSignatureState::Invalid,
                ComponentBundleSignatureVerificationCode::VerifierFailure);
        }
        const void* values[] = {anchor.get()};
        anchors = UniqueCF<CFArrayRef>(CFArrayCreate(
            kCFAllocatorDefault,
            values,
            1,
            &kCFTypeArrayCallBacks));
        if (anchors.get() == nullptr
            || SecTrustSetAnchorCertificates(trust.get(), anchors.get())
                != errSecSuccess
            || SecTrustSetAnchorCertificatesOnly(trust.get(), true)
                != errSecSuccess)
        {
            return failed(
                ComponentSignatureState::Invalid,
                ComponentBundleSignatureVerificationCode::VerifierFailure);
        }
    }

    UniqueCF<CFErrorRef> trustError;
    if (!SecTrustEvaluateWithError(trust.get(), trustError.out()))
    {
        const auto errorCode = trustError.get() != nullptr
            ? CFErrorGetCode(trustError.get())
            : 0;
        if (errorCode == errSecCertificateExpired)
        {
            return failed(
                ComponentSignatureState::Expired,
                ComponentBundleSignatureVerificationCode::ExpiredSigner);
        }
        if (errorCode == errSecCertificateRevoked)
        {
            return failed(
                ComponentSignatureState::Revoked,
                ComponentBundleSignatureVerificationCode::RevokedSigner);
        }
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::UntrustedSigner);
    }

    UniqueCF<SecCertificateRef> signerCertificate;
    if (CMSDecoderCopySignerCert(
            decoder.get(),
            0,
            signerCertificate.out()) != errSecSuccess
        || signerCertificate.get() == nullptr)
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }
    UniqueCF<CFDataRef> certificateDer(
        SecCertificateCopyData(signerCertificate.get()));
    if (certificateDer.get() == nullptr
        || CFDataGetLength(certificateDer.get()) <= 0
        || static_cast<unsigned long long>(
               CFDataGetLength(certificateDer.get()))
            > std::numeric_limits<std::uint32_t>::max())
    {
        return failed(
            ComponentSignatureState::Invalid,
            ComponentBundleSignatureVerificationCode::VerifierFailure);
    }

    ComponentBundleSignatureVerificationResult result;
    result.code = ComponentBundleSignatureVerificationCode::Verified;
    ComponentSignatureEvidenceView evidence;
    evidence.state = ComponentSignatureState::Verified;
    evidence.contentDigest = snapshot.contentDigest;
    evidence.signerIdentityDigest = signerIdentityDigest({
        CFDataGetBytePtr(certificateDer.get()),
        static_cast<std::size_t>(CFDataGetLength(certificateDer.get()))});
    evidence.profileVersion = currentComponentSignatureProfileVersion;
    evidence.hasContentDigest = true;
    evidence.hasSignerIdentity = true;
    result.evidence = ComponentBundleSignatureEvidenceFactory::make(evidence);
    return result;
}
#endif
}

std::vector<std::uint8_t> makeComponentBundleSignatureMessage(
    const std::array<std::uint8_t, componentTrustDigestBytes>& contentDigest)
{
    std::vector<std::uint8_t> message(
        componentBundleSignatureDomain.begin(),
        componentBundleSignatureDomain.end());
    message.reserve(message.size() + 1U + contentDigest.size());
    message.push_back(0);
    message.insert(message.end(), contentDigest.begin(), contentDigest.end());
    return message;
}

ComponentBundleSignatureVerificationResult verifyComponentBundleSignature(
    const ComponentBundleSnapshot& snapshot)
{
#if defined(__APPLE__)
    return verifyMacOS(snapshot, {}, std::nullopt);
#else
    (void)snapshot;
    return failed(
        ComponentSignatureState::Unsupported,
        ComponentBundleSignatureVerificationCode::UnsupportedPlatform);
#endif
}

namespace detail
{
ComponentBundleSignatureVerificationResult
verifyComponentBundleSignatureWithAnchorForTesting(
    const ComponentBundleSnapshot& snapshot,
    std::span<const std::uint8_t> trustedAnchorDer)
{
#if defined(__APPLE__)
    return verifyMacOS(snapshot, trustedAnchorDer, std::nullopt);
#else
    (void)snapshot;
    (void)trustedAnchorDer;
    return failed(
        ComponentSignatureState::Unsupported,
        ComponentBundleSignatureVerificationCode::UnsupportedPlatform);
#endif
}

ComponentBundleSignatureVerificationResult
verifyComponentBundleSignatureAtUnixTimeForTesting(
    const ComponentBundleSnapshot& snapshot,
    std::span<const std::uint8_t> trustedAnchorDer,
    double unixTime)
{
#if defined(__APPLE__)
    return verifyMacOS(snapshot, trustedAnchorDer, unixTime);
#else
    (void)snapshot;
    (void)trustedAnchorDer;
    (void)unixTime;
    return failed(
        ComponentSignatureState::Unsupported,
        ComponentBundleSignatureVerificationCode::UnsupportedPlatform);
#endif
}
}
}
