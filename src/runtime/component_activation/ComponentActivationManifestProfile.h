#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"

#include <cstdint>

namespace talas::runtime::component_activation
{
enum class ComponentActivationMacOSProfileV1 : std::uint8_t;

enum class ComponentActivationManifestProfileV1 : std::uint8_t
{
    Unsupported,
    StatelessAudio,
    ImmutableResource,
    BoundedAudioStream,
    MultipleImmutableResources,
};

[[nodiscard]] ComponentActivationManifestProfileV1
supportedComponentActivationManifestProfileV1(
    const component_bundle::ComponentBundleSnapshot& snapshot,
    std::uint16_t inputChannelCount,
    std::uint16_t outputChannelCount,
    std::uint16_t opaqueStateByteCount) noexcept;

[[nodiscard]] bool componentActivationManifestClaimsMatchRuntimeProfileV1(
    const component_bundle::ComponentBundleSnapshot& snapshot,
    ComponentActivationMacOSProfileV1 profile) noexcept;
}
