#pragma once

#include <array>
#include <cstdint>

namespace talas::runtime::component_probe
{
inline constexpr std::uint32_t componentBundleProbeSandboxReportMagicV1 =
    0x4b435053U;
inline constexpr std::uint16_t componentBundleProbeSandboxReportVersionV1 = 1;
inline constexpr char componentBundleProbeSandboxProjectPathV1[] =
    "/private/tmp/TalasComponentProbeSandbox/Project.talas.json";
inline constexpr char componentBundleProbeSandboxRealtimePathV1[] =
    "/private/tmp/TalasComponentProbeSandbox/Realtime.shmem";
inline constexpr char componentBundleProbeSandboxSubprocessPathV1[] =
    "/private/tmp/TalasComponentProbeSandbox/SpawnProbe";
inline constexpr char componentBundleProbeSandboxDevicePathV1[] = "/dev/disk0";
inline constexpr char componentBundleProbeSandboxUserFileNameV1[] =
    "TalasComponentProbeSandboxUser.txt";
inline constexpr char componentBundleProbeSandboxCredentialServiceV1[] =
    "com.talas.component-probe-sandbox.credential-v1";
inline constexpr char componentBundleProbeSandboxCredentialAccountV1[] =
    "ambient-authority";
inline constexpr std::uint16_t componentBundleProbeSandboxNetworkPortV1 =
    49'322;

enum class ComponentBundleProbeSandboxAuthorityV1 : std::uint32_t
{
    ProjectFile = 1U << 0U,
    UserFile = 1U << 1U,
    OutboundNetwork = 1U << 2U,
    Subprocess = 1U << 3U,
    Device = 1U << 4U,
    Credentials = 1U << 5U,
    RealtimeSharedStorage = 1U << 6U,
    ExistingRealtimeHelper = 1U << 7U,
    SourceBundle = 1U << 8U,
};

inline constexpr std::uint32_t allComponentBundleProbeSandboxAuthoritiesV1 =
    (1U << 9U) - 1U;
inline constexpr std::uint32_t componentBundleProbePrivateCreateV1 = 1U << 0U;
inline constexpr std::uint32_t componentBundleProbePrivateWriteV1 = 1U << 1U;
inline constexpr std::uint32_t componentBundleProbePrivateReadV1 = 1U << 2U;
inline constexpr std::uint32_t componentBundleProbePrivateRemoveV1 = 1U << 3U;
inline constexpr std::uint32_t allComponentBundleProbePrivateOperationsV1 =
    (1U << 4U) - 1U;

struct ComponentBundleProbeSandboxReportV1
{
    std::uint32_t magic = componentBundleProbeSandboxReportMagicV1;
    std::uint16_t version = componentBundleProbeSandboxReportVersionV1;
    std::uint16_t recordBytes = sizeof(ComponentBundleProbeSandboxReportV1);
    std::uint32_t attemptedAuthorities = 0;
    std::uint32_t deniedAuthorities = 0;
    std::uint32_t privateArtifactOperations = 0;
    std::uint32_t reserved0 = 0;
    std::array<std::int32_t, 9> nativeResults{};
    std::array<std::uint64_t, 2> reserved1{};
};

static_assert(sizeof(ComponentBundleProbeSandboxReportV1) == 80);

[[nodiscard]] ComponentBundleProbeSandboxReportV1
runComponentBundleProbeSandboxReportV1() noexcept;
}
