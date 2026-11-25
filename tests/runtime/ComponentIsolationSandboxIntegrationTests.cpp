#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "talas/runtime/ComponentIsolationMacOSHost.h"
#include "runtime/macos/ComponentIsolationSandboxProbe.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path embeddedHelperPath(const char* bundleName)
{
    std::uint32_t bytes = 0;
    require(
        _NSGetExecutablePath(nullptr, &bytes) == -1 && bytes != 0,
        "test harness should resolve its signed app executable");
    std::vector<char> path(bytes);
    require(
        _NSGetExecutablePath(path.data(), &bytes) == 0,
        "test harness executable path should fit the queried buffer");
    const auto executable = std::filesystem::weakly_canonical(path.data());
    return executable.parent_path().parent_path()
        / "XPCServices"
        / bundleName;
}

bool hasExactSandboxSignature(
    const std::filesystem::path& helper,
    CFStringRef expectedIdentifier)
{
    require(
        std::filesystem::is_directory(helper),
        "sandbox profile check: embedded helper bundle is missing");
    const auto path = helper.string();
    auto url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()),
        static_cast<CFIndex>(path.size()),
        true);
    require(url != nullptr, "sandbox profile check: helper URL is invalid");
    SecStaticCodeRef code = nullptr;
    const auto created = SecStaticCodeCreateWithPath(
        url,
        kSecCSDefaultFlags,
        &code);
    CFRelease(url);
    require(
        created == errSecSuccess && code != nullptr,
        "sandbox profile check: helper is unsigned or unreadable");
    require(
        SecStaticCodeCheckValidity(
            code,
            kSecCSStrictValidate,
            nullptr)
            == errSecSuccess,
        "sandbox profile check: helper signature is invalid");

    CFDictionaryRef information = nullptr;
    require(
        SecCodeCopySigningInformation(
            code,
            kSecCSSigningInformation,
            &information)
                == errSecSuccess
            && information != nullptr,
        "sandbox profile check: signing information is unavailable");
    const auto identifier = static_cast<CFStringRef>(CFDictionaryGetValue(
        information,
        kSecCodeInfoIdentifier));
    const auto entitlements = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(
            information,
            kSecCodeInfoEntitlementsDict));
    const auto appSandbox = entitlements == nullptr
        ? nullptr
        : CFDictionaryGetValue(
              entitlements,
              CFSTR("com.apple.security.app-sandbox"));
    const auto valid = identifier != nullptr
        && CFEqual(identifier, expectedIdentifier)
        && entitlements != nullptr
        && CFDictionaryGetCount(entitlements) == 1
        && appSandbox == kCFBooleanTrue;
    CFRelease(information);
    CFRelease(code);
    return valid;
}

int createNetworkSentinel()
{
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, 0);
    require(socketHandle >= 0, "network sentinel socket should open");
    int reuse = 1;
    require(
        setsockopt(
            socketHandle,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse))
            == 0,
        "network sentinel should enable address reuse");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(componentIsolationSandboxNetworkPortV1);
    require(
        bind(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address))
                == 0
            && listen(socketHandle, 1) == 0,
        "network sentinel should bind the fixed loopback probe port");
    return socketHandle;
}

ComponentIsolationControlSessionConfigurationV1 configuration()
{
    ComponentIsolationControlSessionConfigurationV1 value;
    for (std::size_t index = 0;
         index < value.contract.validationNonce.size();
         ++index)
    {
        value.contract.validationNonce[index] =
            static_cast<std::uint8_t>(index + 31);
    }
    value.contract.sessionGeneration = 303;
    value.contract.sharedStorageIdentity = 707;
    value.contract.inputChannelCount = 2;
    value.contract.outputChannelCount = 2;
    value.contract.maximumFrames = 64;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.controlPayloadCapacityBytes = 128;
    return value;
}

bool waitForState(
    const ComponentIsolationMacOSHost& host,
    ComponentIsolationControlSessionState expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (host.state() == expected)
            return true;
        if (host.state() == ComponentIsolationControlSessionState::Quarantined)
            return expected == ComponentIsolationControlSessionState::Quarantined;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testSignedSandboxDeniesAmbientAuthorityAndKeepsXpcShmem()
{
    require(
        !hasExactSandboxSignature(
            embeddedHelperPath("TalasComponentHostUnsandboxed.xpc"),
            CFSTR("com.talas.workbench.component-host")),
        "sandbox profile check should reject a signed helper without the expected profile before launch");
    require(
        hasExactSandboxSignature(
            embeddedHelperPath("TalasComponentHostSandbox.xpc"),
            CFSTR("com.talas.workbench.component-host.fixture.sandbox")),
        "sandbox profile check: helper has missing or unexpected entitlements");

    const auto probeDirectory = std::filesystem::path(
        componentIsolationSandboxProjectPathV1).parent_path();
    std::filesystem::create_directory(probeDirectory);
    {
        std::ofstream project(componentIsolationSandboxProjectPathV1);
        std::ofstream arbitrary(componentIsolationSandboxArbitraryPathV1);
        std::ofstream subprocess(componentIsolationSandboxSubprocessPathV1);
        require(
            project.good() && arbitrary.good() && subprocess.good(),
            "sandbox probe sentinel files should exist before helper launch");
        project << "project sentinel";
        arbitrary << "arbitrary sentinel";
        subprocess << "#!/bin/sh\nexit 0\n";
    }
    require(
        chmod(componentIsolationSandboxSubprocessPathV1, 0700) == 0,
        "sandbox subprocess sentinel should be executable outside the profile");
    const auto networkSentinel = createNetworkSentinel();

    const auto control = configuration();
    ComponentIsolationAudioDataPlaneConfiguration audioConfiguration;
    audioConfiguration.contract = control.contract;
    auto preparedAudio = ComponentIsolationAudioDataPlane::prepare(
        audioConfiguration);
    require(
        preparedAudio.diagnostic
                == ComponentIsolationAudioPreparationDiagnostic::Ready
            && preparedAudio.dataPlane != nullptr,
        "sandbox probe should prepare host-owned mmap shared storage");
    auto sharedAudio = std::shared_ptr<ComponentIsolationAudioDataPlane>(
        std::move(preparedAudio.dataPlane));
    auto preparedHost = ComponentIsolationMacOSHost::prepare(
        control,
        sharedAudio,
        ComponentIsolationMacOSProductEndpointV1::FixtureSandboxProbe);
    require(
        preparedHost.diagnostic
                == ComponentIsolationMacOSHostPreparationDiagnostic::Ready
            && preparedHost.host != nullptr
            && preparedHost.host->start()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *preparedHost.host,
                ComponentIsolationControlSessionState::Ready),
        "sandbox probe should keep bounded XPC and explicit shmem handshake ready");
    require(
        preparedHost.host->requestTeardown()
                == ComponentIsolationMacOSHostOperation::Scheduled
            && waitForState(
                *preparedHost.host,
                ComponentIsolationControlSessionState::Quarantined),
        "sandbox probe should return one bounded typed denial report");

    ComponentIsolationSandboxProbeReportV1 report;
    require(
        preparedHost.host->diagnostic()
                == ComponentIsolationControlSessionDiagnostic::RemoteFailure
            && preparedHost.host->diagnosticBytes() == sizeof(report)
            && preparedHost.host->copyDiagnostic(std::as_writable_bytes(
                   std::span(&report, 1)))
                == sizeof(report),
        "sandbox probe did not execute or return its exact report");
    require(
        report.magic == componentIsolationSandboxProbeMagicV1
            && report.version == componentIsolationSandboxProbeVersionV1
            && report.recordBytes == sizeof(report)
            && report.transportProof
                == componentIsolationSandboxSharedStorageMappedV1
            && report.hostProcessIdentifier == getpid()
            && report.reserved0 == 0,
        "sandbox probe report should bind the tested XPC/shmem session and host");
    if (report.deniedAuthorities != allComponentIsolationSandboxAuthoritiesV1)
    {
        std::cerr << "attempted=" << report.attemptedAuthorities
                  << " denied=" << report.deniedAuthorities << " results=";
        for (const auto result : report.nativeResults)
            std::cerr << result << ',';
        std::cerr << '\n';
    }
    require(
        report.attemptedAuthorities
            == allComponentIsolationSandboxAuthoritiesV1,
        "sandbox probe did not execute every ambient-authority attempt");
    require(
        report.deniedAuthorities
            == allComponentIsolationSandboxAuthoritiesV1,
        "signed App Sandbox helper allowed ambient authority");

    close(networkSentinel);
    std::filesystem::remove(componentIsolationSandboxProjectPathV1);
    std::filesystem::remove(componentIsolationSandboxArbitraryPathV1);
    std::filesystem::remove(componentIsolationSandboxSubprocessPathV1);
    std::filesystem::remove(probeDirectory);
}
}

int main()
{
    testSignedSandboxDeniesAmbientAuthorityAndKeepsXpcShmem();
    return 0;
}
