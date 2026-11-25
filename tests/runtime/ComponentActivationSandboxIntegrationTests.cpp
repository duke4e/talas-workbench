#include "runtime/component_activation/macos/ComponentActivationSandboxReport.h"
#include "runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
namespace activation = talas::runtime::component_activation;
using namespace talas::runtime;

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
        "sandbox harness should resolve its executable");
    std::vector<char> path(bytes);
    require(
        _NSGetExecutablePath(path.data(), &bytes) == 0,
        "sandbox harness executable path should fit");
    return std::filesystem::weakly_canonical(path.data())
        .parent_path()
        .parent_path();
}

bool hasExactSandboxSignature(
    const std::filesystem::path& helper,
    CFStringRef expectedIdentifier)
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
        url, kSecCSDefaultFlags, &code);
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
        code, kSecCSSigningInformation, &information);
    CFRelease(code);
    if (copied != errSecSuccess || information == nullptr)
        return false;
    const auto identifier = static_cast<CFStringRef>(CFDictionaryGetValue(
        information, kSecCodeInfoIdentifier));
    const auto entitlements = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict));
    const auto sandbox = entitlements == nullptr
        ? nullptr
        : CFDictionaryGetValue(
              entitlements, CFSTR("com.apple.security.app-sandbox"));
    const auto disabledValidation = entitlements == nullptr
        ? nullptr
        : CFDictionaryGetValue(
              entitlements,
              CFSTR("com.apple.security.cs.disable-library-validation"));
    const auto valid = identifier != nullptr
        && CFEqual(identifier, expectedIdentifier)
        && entitlements != nullptr
        && CFDictionaryGetCount(entitlements) == 2
        && sandbox == kCFBooleanTrue
        && disabledValidation == kCFBooleanTrue;
    CFRelease(information);
    return valid;
}

int createNetworkSentinel()
{
    const auto handle = socket(AF_INET, SOCK_STREAM, 0);
    require(handle >= 0, "activation sandbox network sentinel should open");
    int reuse = 1;
    require(
        setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))
            == 0,
        "activation sandbox network sentinel should enable reuse");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(
        activation::componentActivationSandboxNetworkPortV1);
    require(
        bind(
            handle,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0
            && listen(handle, 1) == 0,
        "activation sandbox network sentinel should listen");
    return handle;
}

CFMutableDictionaryRef credentialQuery()
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* values[] = {
        kSecClassGenericPassword,
        CFSTR("com.talas.component-activation-sandbox.credential-v1"),
        CFSTR("ambient-authority"),
    };
#pragma clang diagnostic pop
    const auto base = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        static_cast<CFIndex>(std::size(keys)),
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (base == nullptr)
        return nullptr;
    const auto result = CFDictionaryCreateMutableCopy(
        kCFAllocatorDefault, 0, base);
    CFRelease(base);
    return result;
}

void removeCredentialSentinel()
{
    const auto query = credentialQuery();
    require(query != nullptr, "credential cleanup query should allocate");
    const auto result = SecItemDelete(query);
    CFRelease(query);
    require(
        result == errSecSuccess || result == errSecItemNotFound,
        "credential cleanup should succeed");
}

void createCredentialSentinel()
{
    removeCredentialSentinel();
    const auto query = credentialQuery();
    require(query != nullptr, "credential sentinel query should allocate");
    constexpr std::array<std::uint8_t, 4> secret{1, 2, 3, 4};
    auto data = CFDataCreate(
        kCFAllocatorDefault,
        secret.data(),
        static_cast<CFIndex>(secret.size()));
    require(data != nullptr, "credential sentinel data should allocate");
    CFDictionarySetValue(query, kSecValueData, data);
    const auto result = SecItemAdd(query, nullptr);
    CFRelease(data);
    CFRelease(query);
    require(result == errSecSuccess, "credential sentinel should be stored");
}

activation::ComponentActivationSealedSessionV1 sealedSession()
{
    activation::ComponentActivationSealedSessionV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::SealedSession,
        sizeof(value));
    value.profileVersion = activation::componentActivationProfileVersionV1;
    value.supportedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.safeguards = allComponentIsolationSafeguardsV1;
    value.runtimeSessionGeneration = 919;
    value.sharedStorageIdentity = 929;
    value.sharedStorageBytes = 64U * 1024U;
    value.runtimeSessionNonce.fill(0x71);
    return value;
}

activation::ComponentActivationSandboxReportV1 requestReport()
{
    const auto session = sealedSession();
    const auto storageBytes = static_cast<std::size_t>(
        session.sharedStorageBytes);
    auto* storage = mmap(
        nullptr,
        storageBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED,
        -1,
        0);
    require(storage != MAP_FAILED, "activation sandbox shmem should allocate");
    std::memcpy(storage, &session, sizeof(session));
    auto sharedMemory = xpc_shmem_create(storage, storageBytes);
    require(sharedMemory != nullptr, "activation sandbox shmem should wrap");
    auto request = activation::createComponentActivationXpcSealedPrewarmV1(
        session, sharedMemory);
    require(request != nullptr, "activation sandbox prewarm should encode");

    auto queue = dispatch_queue_create(
        "com.talas.component-activation-sandbox-tests.v1",
        DISPATCH_QUEUE_SERIAL);
    auto semaphore = dispatch_semaphore_create(0);
    require(queue != nullptr && semaphore != nullptr,
        "activation sandbox transport primitives should allocate");
    auto connection = xpc_connection_create(
        "com.talas.workbench.component-runtime.fixture.sandbox",
        queue);
    require(connection != nullptr, "activation sandbox XPC should create");
    xpc_connection_set_event_handler(connection, ^(xpc_object_t) {});
    xpc_connection_activate(connection);

    __block bool valid = false;
    __block activation::ComponentActivationSandboxReportV1 report;
    xpc_connection_send_message_with_reply(
        connection,
        request,
        queue,
        ^(xpc_object_t reply) {
            if (reply != nullptr
                && xpc_get_type(reply) == XPC_TYPE_DICTIONARY
                && xpc_dictionary_get_count(reply) == 2)
            {
                std::size_t transitionBytes = 0;
                const auto* transitionData = xpc_dictionary_get_data(
                    reply,
                    "talas.component-activation.transition.v1",
                    &transitionBytes);
                std::size_t reportBytes = 0;
                const auto* reportData = xpc_dictionary_get_data(
                    reply,
                    "talas.component-activation.sandbox-report.v1",
                    &reportBytes);
                if (transitionData != nullptr
                    && transitionBytes
                        == sizeof(activation::ComponentActivationTransitionV1)
                    && reportData != nullptr
                    && reportBytes == sizeof(report))
                {
                    activation::ComponentActivationTransitionV1 transition;
                    std::memcpy(
                        &transition, transitionData, sizeof(transition));
                    std::memcpy(&report, reportData, sizeof(report));
                    valid = activation::validateComponentActivationSealedTransitionV1(
                        transition,
                        activation::ComponentActivationRecordKindV1::SealedReady,
                        session)
                        == activation::ComponentActivationProtocolDiagnosticV1::Valid;
                }
            }
            dispatch_semaphore_signal(semaphore);
        });
    const auto waited = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    xpc_connection_cancel(connection);
    xpc_release(connection);
    xpc_release(request);
    xpc_release(sharedMemory);
    require(munmap(storage, storageBytes) == 0,
        "activation sandbox shmem should release");
#if !OS_OBJECT_USE_OBJC
    dispatch_release(semaphore);
    dispatch_release(queue);
#endif
    require(waited == 0 && valid,
        "activation sandbox should return one exact sealed report");
    return report;
}

void testSealedRuntimeSandboxDeniesAmbientAuthority()
{
    const auto contents = applicationContents();
    require(
        hasExactSandboxSignature(
            contents / "XPCServices"
                / "TalasComponentRuntimeSandbox.xpc",
            CFSTR("com.talas.workbench.component-runtime.fixture.sandbox")),
        "activation runtime helper must have exactly App Sandbox plus disabled library validation");
    require(
        std::filesystem::is_regular_file(
            contents / "XPCServices" / "TalasComponentProbe.xpc"
                / "Contents" / "MacOS" / "TalasComponentProbe")
            && std::filesystem::is_regular_file(
                contents / "XPCServices" / "TalasComponentHost.xpc"
                    / "Contents" / "MacOS" / "TalasComponentHost"),
        "activation sandbox sentinels should be real embedded helpers");

    const auto sentinelDirectory = std::filesystem::path(
        activation::componentActivationSandboxProjectPathV1).parent_path();
    std::filesystem::create_directory(sentinelDirectory);
    {
        std::ofstream project(
            activation::componentActivationSandboxProjectPathV1);
        std::ofstream subprocess(
            activation::componentActivationSandboxSubprocessPathV1);
        require(project.good() && subprocess.good(),
            "activation sandbox sentinel files should exist");
        project << "project";
        subprocess << "#!/bin/sh\nexit 0\n";
    }
    require(
        chmod(
            activation::componentActivationSandboxSubprocessPathV1,
            0700) == 0,
        "activation sandbox subprocess sentinel should be executable");
    const auto network = createNetworkSentinel();
    createCredentialSentinel();

    const auto report = requestReport();
    if (report.deniedAuthorities
        != activation::allComponentActivationSandboxAuthoritiesV1)
    {
        std::cerr << "attempted=" << report.attemptedAuthorities
                  << " denied=" << report.deniedAuthorities << " results=";
        for (const auto result : report.nativeResults)
            std::cerr << result << ',';
        std::cerr << '\n';
    }
    require(
        report.magic == activation::componentActivationSandboxReportMagicV1
            && report.version
                == activation::componentActivationSandboxReportVersionV1
            && report.recordBytes == sizeof(report)
            && report.transportProof
                == activation::componentActivationSandboxSharedStorageMappedV1
            && report.hostProcessIdentifier == getpid()
            && report.reserved0 == std::array<std::uint32_t, 3>{},
        "activation sandbox report should have the exact closed binding");
    require(
        report.attemptedAuthorities
                == activation::allComponentActivationSandboxAuthoritiesV1
            && report.deniedAuthorities
                == activation::allComponentActivationSandboxAuthoritiesV1,
        "sealed activation App Sandbox helper allowed ambient authority");

    close(network);
    removeCredentialSentinel();
    std::filesystem::remove(
        activation::componentActivationSandboxProjectPathV1);
    std::filesystem::remove(
        activation::componentActivationSandboxSubprocessPathV1);
    std::filesystem::remove(sentinelDirectory);
}
}

int main()
{
    testSealedRuntimeSandboxDeniesAmbientAuthority();
    return 0;
}
