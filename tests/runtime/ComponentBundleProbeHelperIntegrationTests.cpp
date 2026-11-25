#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <xpc/xpc.h>

#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH is required
#endif

#ifndef TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH is required
#endif

#ifndef TALAS_COMPONENT_PROBE_UNLINKED_IMAGE_FIXTURE_PATH
#error TALAS_COMPONENT_PROBE_UNLINKED_IMAGE_FIXTURE_PATH is required
#endif

namespace
{
namespace probe = talas::runtime::component_probe;
constexpr char cleanFixtureServiceName[] =
    "com.talas.workbench.component-probe.fixture.local-clean-v1";
constexpr char digestFixtureServiceName[] =
    "com.talas.workbench.component-probe.fixture.local-digest-v1";
constexpr char abiFixtureServiceName[] =
    "com.talas.workbench.component-probe.fixture.local-abi-v1";
constexpr char fatalFixtureServiceName[] =
    "com.talas.workbench.component-probe.fixture.local-fatal-v1";
constexpr char unlinkedFixtureServiceName[] =
    "com.talas.workbench.component-probe.fixture.local-unlinked-v1";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path embeddedProbeHelperPath()
{
    std::uint32_t bytes = 0;
    require(
        _NSGetExecutablePath(nullptr, &bytes) == -1 && bytes != 0,
        "integration app should resolve its executable path");
    std::vector<char> path(bytes);
    require(
        _NSGetExecutablePath(path.data(), &bytes) == 0,
        "integration app executable path should fit");
    return std::filesystem::weakly_canonical(path.data())
        .parent_path()
        .parent_path()
        / "XPCServices"
        / "TalasComponentProbe.xpc";
}

bool hasExactProbeSignature(const std::filesystem::path& helper)
{
    if (!std::filesystem::is_directory(helper))
        return false;
    const auto path = helper.string();
    auto url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()),
        static_cast<CFIndex>(path.size()),
        true);
    if (url == nullptr)
        return false;
    SecStaticCodeRef code = nullptr;
    const auto created = SecStaticCodeCreateWithPath(
        url,
        kSecCSDefaultFlags,
        &code);
    CFRelease(url);
    if (created != errSecSuccess || code == nullptr)
        return false;
    if (SecStaticCodeCheckValidity(code, kSecCSStrictValidate, nullptr)
        != errSecSuccess)
    {
        CFRelease(code);
        return false;
    }
    CFDictionaryRef information = nullptr;
    const auto copied = SecCodeCopySigningInformation(
        code,
        kSecCSSigningInformation,
        &information);
    if (copied != errSecSuccess || information == nullptr)
    {
        CFRelease(code);
        return false;
    }
    const auto identifier = static_cast<CFStringRef>(CFDictionaryGetValue(
        information,
        kSecCodeInfoIdentifier));
    const auto entitlements = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict));
    const auto sandbox = entitlements == nullptr
        ? nullptr
        : CFDictionaryGetValue(
              entitlements,
              CFSTR("com.apple.security.app-sandbox"));
    const auto disabledValidation = entitlements == nullptr
        ? nullptr
        : CFDictionaryGetValue(
              entitlements,
              CFSTR("com.apple.security.cs.disable-library-validation"));
    const auto valid = identifier != nullptr
        && CFEqual(
            identifier,
            CFSTR("com.talas.workbench.component-probe"))
        && entitlements != nullptr
        && CFDictionaryGetCount(entitlements) == 2
        && sandbox == kCFBooleanTrue
        && disabledValidation == kCFBooleanTrue;
    CFRelease(information);
    CFRelease(code);
    return valid;
}

std::vector<std::byte> readBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.good(), "probe fixture should be readable by the host test");
    const auto end = stream.tellg();
    require(end > 0, "probe fixture should contain a Mach-O image");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "probe fixture bytes should read exactly");
    return bytes;
}

std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes> digest(
    std::span<const std::byte> bytes)
{
    std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes> value{};
    require(
        bytes.size() <= std::numeric_limits<CC_LONG>::max()
            && CC_SHA256(
                   bytes.data(),
                   static_cast<CC_LONG>(bytes.size()),
                   value.data()) != nullptr,
        "fixture digest should compute");
    return value;
}

probe::ComponentBundleProbeRequestV1 request(std::size_t entryBytes)
{
    probe::ComponentBundleProbeRequestV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(value));
    constexpr std::string_view bundleId = "org.talas.fixture";
    constexpr std::string_view version = "1.2.3";
    value.bundleIdBytes = bundleId.size();
    value.semanticVersionBytes = version.size();
    std::copy(bundleId.begin(), bundleId.end(), value.bundleId.begin());
    std::copy(version.begin(), version.end(), value.semanticVersion.begin());
    value.contentDigest.fill(0x51);
    value.signerIdentityDigest.fill(0x63);
    value.requestedPermissions = talas::runtime::componentPermission(
        talas::runtime::ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles = talas::runtime::componentRole(
        talas::runtime::ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = talas::runtime::currentComponentTrustPolicyVersion;
    value.entryByteCount = entryBytes;
    value.artifactGeneration = 91;
    value.validationNonce.fill(0x75);
    return value;
}

probe::ComponentBundleProbeExpectedAbiV1 expectedAbi(
    const probe::ComponentBundleProbeRequestV1& requestValue,
    std::span<const std::byte> entryBytes)
{
    probe::ComponentBundleProbeExpectedAbiV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(value));
    value.entryDigest = digest(entryBytes);
    value.entryByteCount = entryBytes.size();
    value.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    constexpr std::string_view nodeId =
        "org.talas.fixture.oscillator";
    value.nodeCount = 1;
    value.nodeIdBytes[0] = nodeId.size();
    std::copy(nodeId.begin(), nodeId.end(), value.nodeIds[0].begin());
    require(
        probe::validateComponentBundleProbeExpectedAbiV1(value, requestValue)
            == probe::ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "integration expected-ABI record should be exact");
    return value;
}

std::filesystem::path probeTemporaryRoot()
{
    std::array<char, PATH_MAX> root{};
    const auto bytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        root.data(),
        root.size());
    require(bytes != 0 && bytes <= root.size(), "temporary root should resolve");
    return root.data();
}

std::set<std::string> probeTemporaryEntries()
{
    std::set<std::string> entries;
    const auto root = probeTemporaryRoot();
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        const auto name = entry.path().filename().string();
        if (name.starts_with("TalasComponentProbe."))
            entries.insert(name);
    }
    return entries;
}

struct ProbeReply
{
    probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1 envelopeDiagnostic =
        probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    probe::ComponentBundleProbeResultV1 result;
    pid_t helperPid = 0;
};

ProbeReply sendProbe(
    const char* selectedServiceName,
    const probe::ComponentBundleProbeRequestV1& requestValue,
    const probe::ComponentBundleProbeExpectedAbiV1& expectedValue,
    std::span<const std::byte> entryBytes)
{
    auto queue = dispatch_queue_create(
        "com.talas.component-probe-integration",
        DISPATCH_QUEUE_SERIAL);
    require(queue != nullptr, "probe reply queue should allocate");
    auto connection = xpc_connection_create(selectedServiceName, queue);
    require(connection != nullptr, "embedded probe service should resolve");
    __block ProbeReply reply;
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        probe::ComponentBundleProbeStartedV1 started;
        const auto process = xpc_connection_get_pid(connection);
        if (reply.helperPid != 0 || process <= 0
            || probe::decodeComponentBundleProbeXpcStartedV1(
                   event,
                   requestValue,
                   started)
                != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            || started.processIdentifier
                != static_cast<std::uint32_t>(process))
        {
            xpc_connection_cancel(connection);
            return;
        }
        reply.helperPid = process;
        probe::ComponentBundleProbeAcknowledgementV1 acknowledgement;
        acknowledgement.header = probe::componentBundleProbeHeaderV1(
            probe::ComponentBundleProbeRecordKindV1::Acknowledgement,
            sizeof(acknowledgement));
        acknowledgement.contentDigest = requestValue.contentDigest;
        acknowledgement.artifactGeneration = requestValue.artifactGeneration;
        acknowledgement.validationNonce = requestValue.validationNonce;
        auto acknowledgementMessage = xpc_dictionary_create_empty();
        if (acknowledgementMessage == nullptr
            || !probe::writeComponentBundleProbeXpcAcknowledgementV1(
                acknowledgementMessage,
                acknowledgement,
                requestValue))
        {
            if (acknowledgementMessage != nullptr)
                xpc_release(acknowledgementMessage);
            xpc_connection_cancel(connection);
            return;
        }
        xpc_connection_send_message(connection, acknowledgementMessage);
        xpc_release(acknowledgementMessage);
    });
    xpc_connection_resume(connection);
    auto message = probe::createComponentBundleProbeXpcRequestV1(
        requestValue,
        expectedValue,
        entryBytes);
    require(message != nullptr, "probe request should encode for transport");
    auto semaphore = dispatch_semaphore_create(0);
    require(semaphore != nullptr, "probe reply semaphore should allocate");
    xpc_connection_send_message_with_reply(
        connection,
        message,
        queue,
        ^(xpc_object_t event) {
            if (xpc_get_type(event) == XPC_TYPE_ERROR)
            {
                auto* description = xpc_copy_description(event);
                if (description != nullptr)
                {
                    std::cerr << "xpc-error=" << description << '\n';
                    std::free(description);
                }
            }
            reply.envelopeDiagnostic =
                probe::decodeComponentBundleProbeXpcReplyV1(
                    event,
                    requestValue,
                    reply.result);
            dispatch_semaphore_signal(semaphore);
        });
    const auto waited = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    xpc_connection_cancel(connection);
    xpc_release(message);
    xpc_release(connection);
#if !OS_OBJECT_USE_OBJC
    dispatch_release(semaphore);
    dispatch_release(queue);
#endif
    if (waited != 0)
        std::cerr << "timed-out-service=" << selectedServiceName << '\n';
    require(waited == 0, "one-shot probe helper should reply within five seconds");
    return reply;
}

void requireExited(pid_t process)
{
    require(process > 0, "probe reply should identify its disposable process");
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        errno = 0;
        if (kill(process, 0) != 0 && errno == ESRCH)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(false, "probe helper process should exit after its sole reply");
}

void requireTerminal(
    const ProbeReply& reply,
    talas::runtime::ComponentProbeState state,
    probe::ComponentBundleProbeResultDiagnosticV1 diagnostic)
{
    if (reply.envelopeDiagnostic
            != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
        || reply.result.state != static_cast<std::uint32_t>(state)
        || reply.result.diagnostic != static_cast<std::uint32_t>(diagnostic))
    {
        std::cerr << "envelope="
                  << static_cast<int>(reply.envelopeDiagnostic)
                  << " state=" << reply.result.state
                  << " diagnostic=" << reply.result.diagnostic
                  << " expected-state=" << static_cast<std::uint32_t>(state)
                  << " expected-diagnostic="
                  << static_cast<std::uint32_t>(diagnostic) << '\n';
    }
    require(
        reply.envelopeDiagnostic
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && reply.result.state == static_cast<std::uint32_t>(state)
            && reply.result.diagnostic
                == static_cast<std::uint32_t>(diagnostic),
        "probe helper returned an unexpected terminal pair");
}

void testCleanRejectedAndControlledPathsAreOneShotAndLeaveNoFiles()
{
    const auto initialEntries = probeTemporaryEntries();
    const auto cleanBytes = readBytes(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH);
    auto requestValue = request(cleanBytes.size());
    auto expectedValue = expectedAbi(requestValue, cleanBytes);
    auto reply = sendProbe(
        cleanFixtureServiceName,
        requestValue,
        expectedValue,
        cleanBytes);
    requireTerminal(
        reply,
        talas::runtime::ComponentProbeState::Clean,
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    requireExited(reply.helperPid);
    require(
        probeTemporaryEntries() == initialEntries,
        "clean probe should remove its private materialized entry");

    expectedValue.entryDigest[0] ^= 0xff;
    reply = sendProbe(
        digestFixtureServiceName,
        requestValue,
        expectedValue,
        cleanBytes);
    requireTerminal(
        reply,
        talas::runtime::ComponentProbeState::Rejected,
        probe::ComponentBundleProbeResultDiagnosticV1::EntryRejected);
    requireExited(reply.helperPid);
    require(
        probeTemporaryEntries() == initialEntries,
        "digest rejection should not leave private materialization");

    expectedValue = expectedAbi(requestValue, cleanBytes);
    constexpr std::string_view wrongNode =
        "org.talas.fixture.wrong";
    expectedValue.nodeIds[0].fill(0);
    expectedValue.nodeIdBytes[0] = wrongNode.size();
    std::copy(
        wrongNode.begin(),
        wrongNode.end(),
        expectedValue.nodeIds[0].begin());
    reply = sendProbe(
        abiFixtureServiceName,
        requestValue,
        expectedValue,
        cleanBytes);
    requireTerminal(
        reply,
        talas::runtime::ComponentProbeState::Rejected,
        probe::ComponentBundleProbeResultDiagnosticV1::AbiRejected);
    requireExited(reply.helperPid);
    require(
        probeTemporaryEntries() == initialEntries,
        "ABI rejection should remove its private materialized entry");

    const auto fatalBytes = readBytes(
        TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH);
    requestValue = request(fatalBytes.size());
    expectedValue = expectedAbi(requestValue, fatalBytes);
    reply = sendProbe(
        fatalFixtureServiceName,
        requestValue,
        expectedValue,
        fatalBytes);
    requireTerminal(
        reply,
        talas::runtime::ComponentProbeState::ControlledFatal,
        probe::ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal);
    requireExited(reply.helperPid);
    require(
        probeTemporaryEntries() == initialEntries,
        "controlled entry failure should remove its private materialized entry");

    // This module reports failure if its private image copy still exists
    // when its entry runs; a hang there would otherwise leave that copy.
    const auto unlinkedBytes = readBytes(
        TALAS_COMPONENT_PROBE_UNLINKED_IMAGE_FIXTURE_PATH);
    requestValue = request(unlinkedBytes.size());
    expectedValue = expectedAbi(requestValue, unlinkedBytes);
    reply = sendProbe(
        unlinkedFixtureServiceName,
        requestValue,
        expectedValue,
        unlinkedBytes);
    requireTerminal(
        reply,
        talas::runtime::ComponentProbeState::Clean,
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    requireExited(reply.helperPid);
    require(
        probeTemporaryEntries() == initialEntries,
        "module code must run only after the private image copy is removed");
}
}

int main()
{
    testCleanRejectedAndControlledPathsAreOneShotAndLeaveNoFiles();
    require(
        hasExactProbeSignature(embeddedProbeHelperPath()),
        "probe helper should have exactly sandbox and library-validation entitlements");
    return 0;
}
