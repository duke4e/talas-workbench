#include "runtime/component_bundle/ComponentBundleInspection.h"

#include <chrono>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
                / ("talas-component-bundle-"
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

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "test fixture should write");
}

std::string manifestJson()
{
    return R"json({
  "format":"talas.component-bundle",
  "version":1,
  "bundleId":"org.example.fixture",
  "semanticVersion":"1.0.0",
  "engine":{"abiMinimum":1,"abiMaximum":1,"apiMinimum":1,"apiMaximum":1},
  "roles":["realtime-processor"],
  "permissions":["realtime-audio"],
  "entry":{"path":"Payload/component.bin","bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
  "files":[],
  "nodes":[{"typeId":"org.example.fixture.node","stateSchemaVersion":1,"inputs":[],"outputs":[],"parameters":[]}],
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

void makeMultiFileBundle(const std::filesystem::path& root)
{
    auto source = manifestJson();
    const auto marker = source.find(R"json("files":[])json");
    require(marker != std::string::npos, "multi-file fixture marker should exist");
    source.replace(
        marker,
        std::string_view(R"json("files":[])json").size(),
        R"json("files":[{"path":"Assets/z.bin","purpose":"resource","bytes":1,"sha256":"594e519ae499312b29433b7dd8a97ff068defcba9755b6d5d00e84c524d67b06"},{"path":"Assets/a.txt","purpose":"license","bytes":1,"sha256":"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"}])json");
    writeText(root / "manifest.json", source);
    writeText(root / "Payload/component.bin", "abc");
    writeText(root / "Assets/z.bin", "z");
    writeText(root / "Assets/a.txt", "a");
}

bool hasCode(
    const bundle::ComponentBundleInspectionResult& result,
    bundle::ComponentBundleInspectionErrorCode code)
{
    for (const auto& error : result.errors)
    {
        if (error.code == code)
            return true;
    }
    return false;
}

std::string hexDigest(
    const std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes>& digest)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string output;
    for (const auto byte : digest)
    {
        output.push_back(hex[byte >> 4U]);
        output.push_back(hex[byte & 0x0fU]);
    }
    return output;
}

void testValidBundleFreezesBytesAndStableIdentity()
{
    TemporaryDirectory directory;
    makeValidBundle(directory.path());

    const auto inspected = bundle::inspectComponentBundle(directory.path());

    require(inspected.ok(), "valid explicit component bundle should inspect");
    require(
        inspected.snapshot->payloads.size() == 1
            && inspected.snapshot->payloads.front().isEntry
            && inspected.snapshot->payloads.front().bytes
                == std::vector<std::uint8_t>({'a', 'b', 'c'})
            && inspected.snapshot->canonicalManifest.starts_with(
                R"json({"format":"talas.component-bundle","version":1)json")
            && !inspected.snapshot->detachedSignatureCms.has_value(),
        "inspection should retain canonical metadata and exact copied payload bytes");
    const auto firstDigest = hexDigest(inspected.snapshot->contentDigest);
    require(
        firstDigest
            == "3b6f6fd0c1f8690e0b2bf5218cea4bfca"
               "0399f2205564c4b8e76087c23149849",
        "content identity should match the locked domain-separated transcript vector");

    writeText(directory.path() / "Payload/component.bin", "changed");
    require(
        inspected.snapshot->payloads.front().bytes
            == std::vector<std::uint8_t>({'a', 'b', 'c'})
            && hexDigest(inspected.snapshot->contentDigest) == firstDigest,
        "accepted snapshot should not retain source-path authority");
}

void testDigestSizeAndUndeclaredEntriesReject()
{
    TemporaryDirectory changed;
    makeValidBundle(changed.path());
    writeText(changed.path() / "Payload/component.bin", "abd");
    const auto digestMismatch = bundle::inspectComponentBundle(changed.path());
    require(
        !digestMismatch.ok()
            && hasCode(digestMismatch, bundle::ComponentBundleInspectionErrorCode::DigestMismatch),
        "same-size changed bytes should fail the declared SHA-256 check");

    TemporaryDirectory resized;
    makeValidBundle(resized.path());
    writeText(resized.path() / "Payload/component.bin", "abcd");
    const auto sizeMismatch = bundle::inspectComponentBundle(resized.path());
    require(
        !sizeMismatch.ok()
            && hasCode(sizeMismatch, bundle::ComponentBundleInspectionErrorCode::SizeMismatch),
        "changed payload size should reject before publication");

    TemporaryDirectory undeclared;
    makeValidBundle(undeclared.path());
    writeText(undeclared.path() / "Assets/extra.bin", "x");
    const auto extra = bundle::inspectComponentBundle(undeclared.path());
    require(
        !extra.ok()
            && hasCode(extra, bundle::ComponentBundleInspectionErrorCode::UndeclaredEntry),
        "undeclared payload files and directories should reject");
}

void testFileOrderAndPurposeAreBoundIntoTheSnapshot()
{
    TemporaryDirectory directory;
    makeMultiFileBundle(directory.path());

    const auto inspected = bundle::inspectComponentBundle(directory.path());

    require(inspected.ok(), "declared bounded resource files should inspect");
    require(
        inspected.snapshot->payloads.size() == 3
            && inspected.snapshot->payloads[0].path == "Assets/a.txt"
            && inspected.snapshot->payloads[0].purpose
                == bundle::ComponentBundleFilePurpose::License
            && inspected.snapshot->payloads[1].path == "Assets/z.bin"
            && inspected.snapshot->payloads[2].path == "Payload/component.bin"
            && inspected.snapshot->payloads[2].isEntry,
        "frozen payloads should use canonical path order and retain closed purposes");
    require(
        hexDigest(inspected.snapshot->contentDigest)
            == "590418dd466ece9aaac315ab8592e6aaf"
               "17988250d5c38603b49225b0718dca3",
        "multi-file content identity should match the locked sorted transcript vector");
}

void testOptionalDetachedSignatureIsFrozenButExcludedFromContentIdentity()
{
    TemporaryDirectory first;
    makeValidBundle(first.path());
    writeText(first.path() / "signature.cms", "first-envelope");

    const auto firstInspection = bundle::inspectComponentBundle(first.path());
    require(firstInspection.ok(), "bounded detached CMS should inspect");
    require(
        firstInspection.snapshot->detachedSignatureCms
            == std::optional<std::vector<std::uint8_t>>({{
                'f', 'i', 'r', 's', 't', '-', 'e', 'n', 'v', 'e', 'l', 'o',
                'p', 'e'}}),
        "inspection should freeze exact detached CMS bytes");

    TemporaryDirectory second;
    makeValidBundle(second.path());
    writeText(second.path() / "signature.cms", "second-envelope");
    const auto secondInspection = bundle::inspectComponentBundle(second.path());
    require(secondInspection.ok(), "a different bounded CMS should inspect");
    require(
        firstInspection.snapshot->contentDigest
                == secondInspection.snapshot->contentDigest
            && firstInspection.snapshot->detachedSignatureCms
                != secondInspection.snapshot->detachedSignatureCms,
        "the detached envelope must not create a circular content digest");

    TemporaryDirectory empty;
    makeValidBundle(empty.path());
    writeText(empty.path() / "signature.cms", {});
    const auto emptyInspection = bundle::inspectComponentBundle(empty.path());
    require(
        emptyInspection.ok()
            && emptyInspection.snapshot->detachedSignatureCms.has_value()
            && emptyInspection.snapshot->detachedSignatureCms->empty(),
        "a present empty CMS must remain distinguishable from no envelope");

    writeText(first.path() / "signature.cms", "mutated");
    require(
        firstInspection.snapshot->detachedSignatureCms->front() == 'f',
        "accepted signature input should not retain source-path authority");
}

void testDetachedSignatureFilesystemAndBudgetRulesReject()
{
    TemporaryDirectory oversized;
    makeValidBundle(oversized.path());
    writeText(
        oversized.path() / "signature.cms",
        std::string(
            bundle::maximumComponentBundleSignatureBytes + 1U,
            'x'));
    const auto overBudget = bundle::inspectComponentBundle(oversized.path());
    require(
        !overBudget.ok()
            && hasCode(
                overBudget,
                bundle::ComponentBundleInspectionErrorCode::BudgetExceeded),
        "oversized detached CMS should reject before publication");

    TemporaryDirectory symlinked;
    makeValidBundle(symlinked.path());
    writeText(symlinked.path() / "outside.cms", "cms");
    std::filesystem::create_symlink(
        symlinked.path() / "outside.cms",
        symlinked.path() / "signature.cms");
    const auto symlink = bundle::inspectComponentBundle(symlinked.path());
    require(
        !symlink.ok()
            && hasCode(
                symlink,
                bundle::ComponentBundleInspectionErrorCode::SymlinkDenied),
        "detached CMS symlinks should reject");
}

void testSymlinkHardlinkAndRootAliasReject()
{
    TemporaryDirectory symlinked;
    makeValidBundle(symlinked.path());
    std::filesystem::remove(symlinked.path() / "Payload/component.bin");
    writeText(symlinked.path() / "outside.bin", "abc");
    std::filesystem::create_symlink(
        symlinked.path() / "outside.bin",
        symlinked.path() / "Payload/component.bin");
    const auto symlink = bundle::inspectComponentBundle(symlinked.path());
    require(
        !symlink.ok()
            && hasCode(symlink, bundle::ComponentBundleInspectionErrorCode::SymlinkDenied),
        "payload symlinks should reject");

    TemporaryDirectory hardlinked;
    makeValidBundle(hardlinked.path());
    std::filesystem::create_hard_link(
        hardlinked.path() / "Payload/component.bin",
        hardlinked.path() / "alias.bin");
    const auto hardlink = bundle::inspectComponentBundle(hardlinked.path());
    require(
        !hardlink.ok()
            && hasCode(hardlink, bundle::ComponentBundleInspectionErrorCode::HardlinkDenied),
        "payloads with mutable hardlink aliases should reject");

    TemporaryDirectory parent;
    const auto real = parent.path() / "real";
    const auto alias = parent.path() / "alias";
    std::filesystem::create_directories(real);
    makeValidBundle(real);
    std::filesystem::create_directory_symlink(real, alias);
    const auto rootAlias = bundle::inspectComponentBundle(alias);
    require(
        !rootAlias.ok()
            && hasCode(rootAlias, bundle::ComponentBundleInspectionErrorCode::SymlinkDenied),
        "a symlinked bundle root should reject");
}

void testFifoEntriesRejectWithoutBlocking()
{
    for (const auto* entry : {"manifest.json", "Payload/component.bin"})
    {
        TemporaryDirectory fifoBundle;
        makeValidBundle(fifoBundle.path());
        const auto fifoPath = fifoBundle.path() / entry;
        std::filesystem::remove(fifoPath);
        require(::mkfifo(fifoPath.c_str(), 0600) == 0, "FIFO fixture should be created");
        const auto started = std::chrono::steady_clock::now();
        const auto fifo = bundle::inspectComponentBundle(fifoBundle.path());
        require(
            !fifo.ok()
                && hasCode(fifo, bundle::ComponentBundleInspectionErrorCode::NonRegularEntry)
                && std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
            std::string("a FIFO ") + entry + " should reject promptly as a non-regular entry");
    }
}

void testCancellationAndConcurrentMutationFailWithoutSnapshot()
{
    TemporaryDirectory cancelled;
    makeValidBundle(cancelled.path());
    const auto cancellation = bundle::inspectComponentBundle(
        cancelled.path(),
        [] { return true; });
    require(
        !cancellation.ok() && !cancellation.snapshot
            && hasCode(cancellation, bundle::ComponentBundleInspectionErrorCode::Cancelled),
        "cooperative cancellation should publish no partial snapshot");

    TemporaryDirectory mutated;
    makeValidBundle(mutated.path());
    bool changed = false;
    int cancellationChecks = 0;
    const auto mutation = bundle::inspectComponentBundle(
        mutated.path(),
        [&] {
            ++cancellationChecks;
            if (!changed && cancellationChecks == 5)
            {
                changed = true;
                std::ofstream output(
                    mutated.path() / "Payload/component.bin",
                    std::ios::binary | std::ios::app);
                output.put('x');
            }
            return false;
        });
    require(
        !mutation.ok() && !mutation.snapshot
            && hasCode(
                mutation,
                bundle::ComponentBundleInspectionErrorCode::ConcurrentMutation),
        "mutation during the admitted read should publish no snapshot");

    TemporaryDirectory replaced;
    makeValidBundle(replaced.path());
    writeText(replaced.path() / "signature.cms", "cms");
    bool swapped = false;
    int replacementChecks = 0;
    const auto replacement = bundle::inspectComponentBundle(
        replaced.path(),
        [&] {
            ++replacementChecks;
            if (!swapped && replacementChecks == 6)
            {
                swapped = true;
                const auto staged =
                    replaced.path() / "Payload/replacement.bin";
                writeText(staged, "abc");
                std::filesystem::rename(
                    staged,
                    replaced.path() / "Payload/component.bin");
            }
            return false;
        });
    require(
        swapped && !replacement.ok() && !replacement.snapshot
            && hasCode(
                replacement,
                bundle::ComponentBundleInspectionErrorCode::ConcurrentMutation),
        "same-name inode replacement after a read must publish no snapshot");
}
}

int main()
{
    testValidBundleFreezesBytesAndStableIdentity();
    testDigestSizeAndUndeclaredEntriesReject();
    testFileOrderAndPurposeAreBoundIntoTheSnapshot();
    testOptionalDetachedSignatureIsFrozenButExcludedFromContentIdentity();
    testDetachedSignatureFilesystemAndBudgetRulesReject();
    testSymlinkHardlinkAndRootAliasReject();
    testFifoEntriesRejectWithoutBlocking();
    testCancellationAndConcurrentMutationFailWithoutSnapshot();
    return 0;
}
