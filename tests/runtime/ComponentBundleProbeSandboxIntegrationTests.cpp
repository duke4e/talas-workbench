#include "runtime/component_probe/macos/ComponentBundleProbeSandboxReport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <xpc/xpc.h>

#include <arpa/inet.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace talas::runtime::component_probe;
constexpr char serviceName[] =
    "com.talas.workbench.component-probe.fixture.sandbox-v1";
constexpr char reportKey[] = "talas.component-probe.sandbox-report.v1";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path applicationContents()
{
    std::uint32_t bytes = 0;
    require(
        _NSGetExecutablePath(nullptr, &bytes) == -1 && bytes != 0,
        "sandbox integration app should resolve its executable");
    std::vector<char> path(bytes);
    require(
        _NSGetExecutablePath(path.data(), &bytes) == 0,
        "sandbox integration executable path should fit");
    return std::filesystem::weakly_canonical(path.data())
        .parent_path()
        .parent_path();
}

bool exactSandboxSignature(const std::filesystem::path& helper)
{
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
            CFSTR("com.talas.workbench.component-probe.fixture.sandbox-v1"))
        && entitlements != nullptr
        && CFDictionaryGetCount(entitlements) == 2
        && sandbox == kCFBooleanTrue
        && disabledValidation == kCFBooleanTrue;
    CFRelease(information);
    CFRelease(code);
    return valid;
}

std::filesystem::path userSentinelPath()
{
    const auto* account = getpwuid(getuid());
    require(
        account != nullptr && account->pw_dir != nullptr,
        "sandbox test should resolve the current user directory");
    return std::filesystem::path(account->pw_dir)
        / componentBundleProbeSandboxUserFileNameV1;
}

int createNetworkSentinel()
{
    const auto handle = socket(AF_INET, SOCK_STREAM, 0);
    require(handle >= 0, "sandbox network sentinel should open");
    int reuse = 1;
    require(
        setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0,
        "sandbox network sentinel should enable reuse");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(componentBundleProbeSandboxNetworkPortV1);
    require(
        bind(
            handle,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0
            && listen(handle, 1) == 0,
        "sandbox network sentinel should listen on the fixed port");
    return handle;
}

CFMutableDictionaryRef credentialQuery()
{
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* values[] = {
        kSecClassGenericPassword,
        CFSTR("com.talas.component-probe-sandbox.credential-v1"),
        CFSTR("ambient-authority"),
    };
    const auto base = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        static_cast<CFIndex>(std::size(keys)),
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (base == nullptr)
        return nullptr;
    const auto mutableCopy = CFDictionaryCreateMutableCopy(
        kCFAllocatorDefault,
        0,
        base);
    CFRelease(base);
    return mutableCopy;
}

void removeCredentialSentinel()
{
    const auto query = credentialQuery();
    require(query != nullptr, "credential query should allocate");
    const auto removed = SecItemDelete(query);
    CFRelease(query);
    require(
        removed == errSecSuccess || removed == errSecItemNotFound,
        "credential sentinel cleanup should succeed");
}

void createCredentialSentinel()
{
    removeCredentialSentinel();
    const auto query = credentialQuery();
    require(query != nullptr, "credential sentinel should allocate");
    constexpr std::array<std::uint8_t, 16> secret{
        0x6b, 0x63, 0x2d, 0x70, 0x72, 0x6f, 0x62, 0x65,
        0x2d, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x31,
    };
    const auto data = CFDataCreate(
        kCFAllocatorDefault,
        secret.data(),
        static_cast<CFIndex>(secret.size()));
    require(data != nullptr, "credential value should allocate");
    CFDictionarySetValue(query, kSecValueData, data);
    const auto added = SecItemAdd(query, nullptr);
    CFRelease(data);
    CFRelease(query);
    require(added == errSecSuccess, "credential sentinel should be stored");
}

ComponentBundleProbeSandboxReportV1 requestReport()
{
    auto queue = dispatch_queue_create(
        "com.talas.component-probe-sandbox-test",
        DISPATCH_QUEUE_SERIAL);
    auto connection = xpc_connection_create(serviceName, queue);
    require(
        queue != nullptr && connection != nullptr,
        "sandbox fixture XPC connection should allocate");
    xpc_connection_set_event_handler(connection, ^(xpc_object_t) {});
    xpc_connection_resume(connection);
    auto request = xpc_dictionary_create_empty();
    auto semaphore = dispatch_semaphore_create(0);
    __block ComponentBundleProbeSandboxReportV1 report;
    __block bool valid = false;
    xpc_connection_send_message_with_reply(
        connection,
        request,
        queue,
        ^(xpc_object_t reply) {
            if (reply != nullptr && xpc_get_type(reply) == XPC_TYPE_DICTIONARY
                && xpc_dictionary_get_count(reply) == 1)
            {
                std::size_t bytes = 0;
                const auto* data = xpc_dictionary_get_data(
                    reply,
                    reportKey,
                    &bytes);
                if (data != nullptr && bytes == sizeof(report))
                {
                    std::memcpy(&report, data, bytes);
                    valid = true;
                }
            }
            dispatch_semaphore_signal(semaphore);
        });
    const auto waited = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    xpc_connection_cancel(connection);
    xpc_release(request);
    xpc_release(connection);
#if !OS_OBJECT_USE_OBJC
    dispatch_release(semaphore);
    dispatch_release(queue);
#endif
    require(
        waited == 0 && valid,
        "sandbox fixture should return one exact bounded report");
    return report;
}

void testExactProfileDeniesAmbientAuthority()
{
    const auto contents = applicationContents();
    require(
        exactSandboxSignature(
            contents / "XPCServices"
                / "TalasComponentProbeSandboxFixture.xpc"),
        "sandbox fixture should carry exactly the product probe profile");
    require(
        std::filesystem::is_regular_file(
            contents / "XPCServices" / "TalasComponentHost.xpc"
                / "Contents" / "MacOS" / "TalasComponentHost"),
        "existing realtime helper sentinel should be an actual embedded helper");

    const auto sentinelDirectory = std::filesystem::path(
        componentBundleProbeSandboxProjectPathV1).parent_path();
    std::filesystem::create_directory(sentinelDirectory);
    const auto userFile = userSentinelPath();
    {
        std::ofstream project(componentBundleProbeSandboxProjectPathV1);
        std::ofstream realtime(componentBundleProbeSandboxRealtimePathV1);
        std::ofstream subprocess(componentBundleProbeSandboxSubprocessPathV1);
        std::ofstream user(userFile);
        require(
            project.good() && realtime.good() && subprocess.good() && user.good(),
            "ambient sandbox sentinel files should exist before launch");
        project << "project";
        realtime << "realtime";
        subprocess << "#!/bin/sh\nexit 0\n";
        user << "user";
    }
    require(
        chmod(componentBundleProbeSandboxSubprocessPathV1, 0700) == 0,
        "sandbox subprocess sentinel should be executable outside the profile");
    createCredentialSentinel();
    const auto network = createNetworkSentinel();

    const auto report = requestReport();
    if (report.deniedAuthorities != allComponentBundleProbeSandboxAuthoritiesV1)
    {
        std::cerr << "attempted=" << report.attemptedAuthorities
                  << " denied=" << report.deniedAuthorities << " results=";
        for (const auto result : report.nativeResults)
            std::cerr << result << ',';
        std::cerr << '\n';
    }
    require(
        report.magic == componentBundleProbeSandboxReportMagicV1
            && report.version == componentBundleProbeSandboxReportVersionV1
            && report.recordBytes == sizeof(report)
            && report.reserved0 == 0
            && report.reserved1 == std::array<std::uint64_t, 2>{},
        "sandbox helper report should have the exact closed layout");
    require(
        report.attemptedAuthorities
                == allComponentBundleProbeSandboxAuthoritiesV1
            && report.deniedAuthorities
                == allComponentBundleProbeSandboxAuthoritiesV1,
        "probe sandbox should deny every ambient authority");
    require(
        report.privateArtifactOperations
            == allComponentBundleProbePrivateOperationsV1,
        "probe sandbox should allow and remove only its private temp artifact");

    close(network);
    std::filesystem::remove(componentBundleProbeSandboxProjectPathV1);
    std::filesystem::remove(componentBundleProbeSandboxRealtimePathV1);
    std::filesystem::remove(componentBundleProbeSandboxSubprocessPathV1);
    std::filesystem::remove(sentinelDirectory);
    std::filesystem::remove(userFile);
    removeCredentialSentinel();
}
}

int main()
{
    testExactProfileDeniesAmbientAuthority();
    return 0;
}
