#pragma once

#include <array>
#include <cstdint>

namespace talas::runtime::component_activation
{
inline constexpr std::uint32_t componentActivationSandboxReportMagicV1 =
    0x4b434153U;
inline constexpr std::uint16_t componentActivationSandboxReportVersionV1 = 1;
inline constexpr char componentActivationSandboxProjectPathV1[] =
    "/private/tmp/TalasComponentActivationSandbox/Project.talas.json";
inline constexpr char componentActivationSandboxSubprocessPathV1[] =
    "/private/tmp/TalasComponentActivationSandbox/SpawnProbe";
inline constexpr char componentActivationSandboxDevicePathV1[] = "/dev/disk0";
inline constexpr char componentActivationSandboxCredentialServiceV1[] =
    "com.talas.component-activation-sandbox.credential-v1";
inline constexpr char componentActivationSandboxCredentialAccountV1[] =
    "ambient-authority";
inline constexpr std::uint16_t componentActivationSandboxNetworkPortV1 =
    49'323;
inline constexpr std::uint32_t componentActivationSandboxSharedStorageMappedV1 =
    1U;

enum class ComponentActivationSandboxAuthorityV1 : std::uint32_t
{
    ProjectFile = 1U << 0U,
    ProbeHelper = 1U << 1U,
    ExistingRealtimeHelper = 1U << 2U,
    SourceBundle = 1U << 3U,
    OutboundNetwork = 1U << 4U,
    Subprocess = 1U << 5U,
    Device = 1U << 6U,
    Credentials = 1U << 7U,
    HostTask = 1U << 8U,
};

inline constexpr std::uint32_t allComponentActivationSandboxAuthoritiesV1 =
    (1U << 9U) - 1U;

struct ComponentActivationSandboxReportV1
{
    std::uint32_t magic = componentActivationSandboxReportMagicV1;
    std::uint16_t version = componentActivationSandboxReportVersionV1;
    std::uint16_t recordBytes = sizeof(ComponentActivationSandboxReportV1);
    std::uint32_t attemptedAuthorities = 0;
    std::uint32_t deniedAuthorities = 0;
    std::uint32_t transportProof = 0;
    std::int32_t hostProcessIdentifier = 0;
    std::array<std::int32_t, 9> nativeResults{};
    std::array<std::uint32_t, 3> reserved0{};
};

static_assert(sizeof(ComponentActivationSandboxReportV1) == 72);

[[nodiscard]] ComponentActivationSandboxReportV1
runComponentActivationSandboxReportV1(
    std::int32_t hostProcessIdentifier) noexcept;
}
