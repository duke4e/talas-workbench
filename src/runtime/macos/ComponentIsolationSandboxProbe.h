#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace talas::runtime
{
inline constexpr std::uint32_t componentIsolationSandboxProbeMagicV1 =
    0x4b435350U;
inline constexpr std::uint16_t componentIsolationSandboxProbeVersionV1 = 1;
inline constexpr char componentIsolationSandboxProjectPathV1[] =
    "/private/tmp/TalasComponentIsolationSandboxProbe/Project.talas.json";
inline constexpr char componentIsolationSandboxArbitraryPathV1[] =
    "/private/tmp/TalasComponentIsolationSandboxProbe/Arbitrary.txt";
inline constexpr char componentIsolationSandboxSubprocessPathV1[] =
    "/private/tmp/TalasComponentIsolationSandboxProbe/SpawnProbe";
inline constexpr char componentIsolationSandboxDevicePathV1[] = "/dev/disk0";
inline constexpr std::uint16_t componentIsolationSandboxNetworkPortV1 =
    49'321;

enum class ComponentIsolationSandboxAuthorityV1 : std::uint32_t
{
    ProjectPath = 1U << 0U,
    ArbitraryFile = 1U << 1U,
    OutboundNetwork = 1U << 2U,
    Subprocess = 1U << 3U,
    Device = 1U << 4U,
    HostTask = 1U << 5U,
};

inline constexpr std::uint32_t allComponentIsolationSandboxAuthoritiesV1 =
    (1U << 6U) - 1U;
inline constexpr std::uint32_t
    componentIsolationSandboxSharedStorageMappedV1 = 1U;

struct ComponentIsolationSandboxProbeReportV1
{
    std::uint32_t magic = componentIsolationSandboxProbeMagicV1;
    std::uint16_t version = componentIsolationSandboxProbeVersionV1;
    std::uint16_t recordBytes = sizeof(ComponentIsolationSandboxProbeReportV1);
    std::uint32_t attemptedAuthorities = 0;
    std::uint32_t deniedAuthorities = 0;
    std::uint32_t transportProof = 0;
    std::int32_t hostProcessIdentifier = 0;
    std::array<std::int32_t, 6> nativeResults{};
    std::uint32_t reserved0 = 0;
};

[[nodiscard]] ComponentIsolationSandboxProbeReportV1
runComponentIsolationSandboxProbeV1(
    std::int32_t hostProcessIdentifier) noexcept;
}
