#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"

#include "talas/project/ProjectAssetInspection.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace
{
namespace bundle = talas::runtime::component_bundle;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-component-signature-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeBytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "test fixture should write exact bytes");
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    writeBytes(
        path,
        std::span(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size()));
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void runOpenSsl(const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>("/usr/bin/openssl"));
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    require(
        posix_spawn_file_actions_init(&actions) == 0,
        "test fixture should initialize process actions");
    require(
        posix_spawn_file_actions_addopen(
            &actions,
            STDOUT_FILENO,
            "/dev/null",
            O_WRONLY,
            0) == 0
            && posix_spawn_file_actions_addopen(
                &actions,
                STDERR_FILENO,
                "/dev/null",
                O_WRONLY,
                0) == 0,
        "test fixture should suppress command output");

    pid_t process = 0;
    const auto spawnStatus = ::posix_spawn(
        &process,
        "/usr/bin/openssl",
        &actions,
        nullptr,
        argv.data(),
        environ);
    posix_spawn_file_actions_destroy(&actions);
    require(spawnStatus == 0, "test fixture should launch OpenSSL");

    int status = 0;
    require(
        ::waitpid(process, &status, 0) == process
            && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "test fixture OpenSSL command should succeed");
}

std::string manifestJson()
{
    return R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.example.signed-fixture",
  "semanticVersion":"1.0.0",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":1,"apiMaximum":1},
  "roles":["realtime-processor"],
  "permissions":["realtime-audio"],
  "entry":{"path":"Payload/component.bin","bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
  "files":[],
  "nodes":[{"typeId":"org.example.signed-fixture.node","stateSchemaVersion":1,"inputs":[],"outputs":[],"parameters":[]}],
  "dependencies":[],
  "state":{"currentSchemaVersion":1,"migrations":[]},
  "provenance":{"publisher":"Example","licenseExpression":"Apache-2.0","notices":[]},
  "declarativeUiPath":null
})json";
}

void makeValidBundle(const std::filesystem::path& root)
{
    writeText(root / "manifest.json", manifestJson());
    writeText(root / "Payload/component.bin", "abc");
}

void makeValidBundleVersion(
    const std::filesystem::path& root,
    std::string_view version)
{
    auto source = manifestJson();
    const auto oldVersion = std::string_view("\"semanticVersion\":\"1.0.0\"");
    const auto offset = source.find(oldVersion);
    require(offset != std::string::npos, "version fixture marker should exist");
    source.replace(
        offset,
        oldVersion.size(),
        "\"semanticVersion\":\"" + std::string(version) + "\"");
    writeText(root / "manifest.json", source);
    writeText(root / "Payload/component.bin", "abc");
}

struct TestSigner
{
    std::filesystem::path certificatePem;
    std::filesystem::path privateKeyPem;
    std::filesystem::path certificateDer;
};

TestSigner makeSigner(
    const std::filesystem::path& directory,
    std::string_view name)
{
    std::filesystem::create_directories(directory);
    TestSigner signer{
        directory / "certificate.pem",
        directory / "private-key.pem",
        directory / "certificate.der"};
    runOpenSsl({
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-sha256",
        "-nodes",
        "-subj",
        "/CN=" + std::string(name),
        "-keyout",
        signer.privateKeyPem.string(),
        "-out",
        signer.certificatePem.string(),
        "-days",
        "3650"});
    runOpenSsl({
        "x509",
        "-in",
        signer.certificatePem.string(),
        "-outform",
        "DER",
        "-out",
        signer.certificateDer.string()});
    return signer;
}

void signDetached(
    const std::filesystem::path& message,
    const std::filesystem::path& output,
    const std::vector<TestSigner>& signers,
    bool attachContent = false)
{
    std::vector<std::string> arguments{
        "cms", "-sign", "-binary", "-in", message.string()};
    for (const auto& signer : signers)
    {
        arguments.insert(
            arguments.end(),
            {"-signer",
             signer.certificatePem.string(),
             "-inkey",
             signer.privateKeyPem.string()});
    }
    arguments.insert(
        arguments.end(),
        {"-outform", "DER", "-out", output.string(), "-nosmimecap"});
    if (attachContent)
        arguments.push_back("-nodetach");
    runOpenSsl(arguments);
}

bundle::ComponentBundleSnapshot inspectUnsigned(
    const std::filesystem::path& root)
{
    makeValidBundle(root);
    const auto result = bundle::inspectComponentBundle(root);
    require(result.ok(), "unsigned component fixture should inspect");
    return *result.snapshot;
}

bundle::ComponentBundleSnapshot installSignatureAndInspect(
    const std::filesystem::path& root,
    const std::filesystem::path& signature)
{
    writeBytes(root / "signature.cms", readBytes(signature));
    const auto result = bundle::inspectComponentBundle(root);
    require(result.ok(), "signed component fixture should inspect");
    return *result.snapshot;
}

void testMissingAndMalformedEnvelopeMapToClosedEvidence()
{
    TemporaryDirectory directory;
    auto snapshot = inspectUnsigned(directory.path());
    const auto missing = bundle::verifyComponentBundleSignature(snapshot);
    require(
        missing.code
                == bundle::ComponentBundleSignatureVerificationCode::MissingEnvelope
            && missing.evidence == talas::runtime::ComponentSignatureEvidence{},
        "missing CMS should produce structurally empty Missing evidence");

    snapshot.detachedSignatureCms = std::vector<std::uint8_t>{0x01, 0x02};
    const auto malformed = bundle::verifyComponentBundleSignature(snapshot);
    require(
        malformed.code
                == bundle::ComponentBundleSignatureVerificationCode::MalformedEnvelope
            && malformed.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Invalid,
        "malformed CMS should map to bounded invalid evidence");

    snapshot.detachedSignatureCms = std::vector<std::uint8_t>{};
    const auto empty = bundle::verifyComponentBundleSignature(snapshot);
    require(
        empty.code
                == bundle::ComponentBundleSignatureVerificationCode::MalformedEnvelope
            && empty.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Invalid,
        "present empty CMS must be invalid rather than missing");
}

void testVerifiedEvidenceBindsContentSignerAndProfile()
{
    TemporaryDirectory directory;
    const auto root = directory.path() / "bundle";
    const auto unsignedSnapshot = inspectUnsigned(root);
    const auto message = bundle::makeComponentBundleSignatureMessage(
        unsignedSnapshot.contentDigest);
    const auto messagePath = directory.path() / "message.bin";
    writeBytes(messagePath, message);
    const auto signer = makeSigner(directory.path() / "signer", "KC Signer A");
    const auto signaturePath = directory.path() / "signature.der";
    signDetached(messagePath, signaturePath, {signer});
    const auto snapshot = installSignatureAndInspect(root, signaturePath);
    const auto certificateDer = readBytes(signer.certificateDer);

    const auto verified =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            snapshot,
            certificateDer);
    const auto verifiedEvidence = verified.evidence.inspect();
    require(
        verified.ok()
            && verifiedEvidence.contentDigest == snapshot.contentDigest
            && verifiedEvidence.profileVersion
                == talas::runtime::currentComponentSignatureProfileVersion
            && verifiedEvidence.hasContentDigest
            && verifiedEvidence.hasSignerIdentity,
        "verified evidence should bind exact content and verifier profile");

    std::vector<std::uint8_t> principalTranscript(
        bundle::componentBundleSignerIdentityDomain.begin(),
        bundle::componentBundleSignerIdentityDomain.end());
    principalTranscript.push_back(0);
    const auto certificateBytes = certificateDer.size();
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        principalTranscript.push_back(static_cast<std::uint8_t>(
            certificateBytes >> shift));
    }
    principalTranscript.insert(
        principalTranscript.end(),
        certificateDer.begin(),
        certificateDer.end());
    require(
        talas::project::calculateProjectAssetSha256(principalTranscript)
            == [&] {
                constexpr std::string_view hex = "0123456789abcdef";
                std::string value;
                for (const auto byte : verifiedEvidence.signerIdentityDigest)
                {
                    value.push_back(hex[byte >> 4U]);
                    value.push_back(hex[byte & 0x0fU]);
                }
                return value;
            }(),
        "signer identity should be the versioned exact leaf-DER principal digest");

    const auto systemTrust = bundle::verifyComponentBundleSignature(snapshot);
    require(
        systemTrust.code
                == bundle::ComponentBundleSignatureVerificationCode::UntrustedSigner
            && systemTrust.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Invalid,
        "a test-only anchor must not affect production system trust");

    auto changedDigest = snapshot;
    changedDigest.contentDigest.front() ^= 0xffU;
    const auto mismatch =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            changedDigest,
            certificateDer);
    require(
        mismatch.code
                == bundle::ComponentBundleSignatureVerificationCode::SnapshotIdentityMismatch
            && mismatch.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Invalid,
        "fabricated aggregate identity must reject before signature verification");

    auto changedPayload = snapshot;
    changedPayload.payloads.front().bytes.front() ^= 0xffU;
    const auto changedPayloadResult =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            changedPayload,
            certificateDer);
    require(
        changedPayloadResult.code
            == bundle::ComponentBundleSignatureVerificationCode::SnapshotIdentityMismatch,
        "payload bytes must be rehashed before Verified evidence is published");

    const auto otherRoot = directory.path() / "other-bundle";
    makeValidBundleVersion(otherRoot, "1.0.1");
    const auto otherSnapshot = installSignatureAndInspect(otherRoot, signaturePath);
    const auto otherContent =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            otherSnapshot,
            certificateDer);
    require(
        otherContent.code
                == bundle::ComponentBundleSignatureVerificationCode::InvalidSignature
            && otherContent.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Invalid,
        "a valid snapshot with another aggregate digest must fail CMS verification");

    const auto expired =
        bundle::detail::verifyComponentBundleSignatureAtUnixTimeForTesting(
            snapshot,
            certificateDer,
            4102444800.0);
    require(
        expired.code
                == bundle::ComponentBundleSignatureVerificationCode::ExpiredSigner
            && expired.evidence.inspect().state
                == talas::runtime::ComponentSignatureState::Expired,
        "offline trust should map reproducible certificate expiry explicitly");
}

void testAttachedMultipleAndSignerReplacementRejectOrChangeIdentity()
{
    TemporaryDirectory directory;
    const auto root = directory.path() / "bundle";
    const auto unsignedSnapshot = inspectUnsigned(root);
    const auto message = bundle::makeComponentBundleSignatureMessage(
        unsignedSnapshot.contentDigest);
    const auto messagePath = directory.path() / "message.bin";
    writeBytes(messagePath, message);
    const auto signerA = makeSigner(directory.path() / "a", "KC Signer A");
    const auto signerB = makeSigner(directory.path() / "b", "KC Signer B");

    const auto attachedPath = directory.path() / "attached.der";
    signDetached(messagePath, attachedPath, {signerA}, true);
    auto snapshot = installSignatureAndInspect(root, attachedPath);
    const auto attached =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            snapshot,
            readBytes(signerA.certificateDer));
    require(
        attached.code
            == bundle::ComponentBundleSignatureVerificationCode::AttachedContentDenied,
        "CMS content must be detached so verification input is unambiguous");

    const auto multiplePath = directory.path() / "multiple.der";
    signDetached(messagePath, multiplePath, {signerA, signerB});
    snapshot = installSignatureAndInspect(root, multiplePath);
    const auto multiple = bundle::verifyComponentBundleSignature(snapshot);
    require(
        multiple.code
            == bundle::ComponentBundleSignatureVerificationCode::MultipleSigners,
        "ambiguous multi-signer CMS should reject before trust evaluation");

    const auto firstPath = directory.path() / "first.der";
    signDetached(messagePath, firstPath, {signerA});
    snapshot = installSignatureAndInspect(root, firstPath);
    const auto first =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            snapshot,
            readBytes(signerA.certificateDer));
    const auto secondPath = directory.path() / "second.der";
    signDetached(messagePath, secondPath, {signerB});
    snapshot = installSignatureAndInspect(root, secondPath);
    const auto second =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            snapshot,
            readBytes(signerB.certificateDer));
    require(
        first.ok() && second.ok()
            && first.evidence.inspect().signerIdentityDigest
                != second.evidence.inspect().signerIdentityDigest,
        "re-signing identical content with another certificate must change identity");
}
}

int main()
{
    testMissingAndMalformedEnvelopeMapToClosedEvidence();
    testVerifiedEvidenceBindsContentSignerAndProfile();
    testAttachedMultipleAndSignerReplacementRejectOrChangeIdentity();
    return 0;
}
