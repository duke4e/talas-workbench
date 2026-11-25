#include "runtime/component_activation/ComponentActivationManifestProfile.h"
#include "runtime/component_activation/macos/ComponentActivationMacOSHost.h"

#include "talas/runtime/ComponentActivationProtocol.h"

#include <algorithm>
#include <ranges>

namespace talas::runtime::component_activation
{
namespace
{
[[nodiscard]] bool knownParameterKind(
    talas::core::ParameterKind kind) noexcept
{
    switch (kind)
    {
        case talas::core::ParameterKind::Expression:
        case talas::core::ParameterKind::Choice:
        case talas::core::ParameterKind::Boolean:
        case talas::core::ParameterKind::Text:
        case talas::core::ParameterKind::Resource:
            return true;
    }
    return false;
}
}

ComponentActivationManifestProfileV1
supportedComponentActivationManifestProfileV1(
    const component_bundle::ComponentBundleSnapshot& snapshot,
    std::uint16_t inputChannelCount,
    std::uint16_t outputChannelCount,
    std::uint16_t opaqueStateByteCount) noexcept
{
    const auto& manifest = snapshot.manifest;
    const auto realtimeRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    const auto resourceRoles = realtimeRoles
        | componentRole(ComponentExecutionRole::ImmutableResourceConsumer);
    const auto audioStreamRoles = realtimeRoles
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto resourceSetRoles = resourceRoles
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto realtimePermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    const auto resourcePermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioStreamPermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto resourceSetPermissions = resourcePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto resourceCount = manifest.nodes.empty()
        ? std::ptrdiff_t{0}
        : std::ranges::count_if(
            manifest.nodes.front().parameters,
            [](const auto& parameter) {
                return parameter.kind == talas::core::ParameterKind::Resource;
            });
    const auto severalResourceProfile = resourceCount >= 2
        && ((manifest.declaredRoles == resourceRoles
                && manifest.requestedPermissions == resourcePermissions)
            || (manifest.declaredRoles == audioStreamRoles
                && manifest.requestedPermissions == audioStreamPermissions)
            || (manifest.declaredRoles == resourceSetRoles
                && manifest.requestedPermissions == resourceSetPermissions));
    const auto profile = severalResourceProfile
        ? ComponentActivationManifestProfileV1::MultipleImmutableResources
        : manifest.declaredRoles == realtimeRoles
            && manifest.requestedPermissions == realtimePermissions
        ? ComponentActivationManifestProfileV1::StatelessAudio
        : manifest.declaredRoles == resourceRoles
                && manifest.requestedPermissions == resourcePermissions
            ? ComponentActivationManifestProfileV1::ImmutableResource
        : manifest.declaredRoles == audioStreamRoles
                && manifest.requestedPermissions == audioStreamPermissions
            ? ComponentActivationManifestProfileV1::BoundedAudioStream
        : manifest.declaredRoles == resourceSetRoles
                && manifest.requestedPermissions == resourceSetPermissions
            ? ComponentActivationManifestProfileV1::MultipleImmutableResources
            : ComponentActivationManifestProfileV1::Unsupported;
    if (profile == ComponentActivationManifestProfileV1::Unsupported
        || manifest.nodes.size() != 1 || !manifest.files.empty()
        || !manifest.dependencies.empty() || !manifest.state.migrations.empty()
        || manifest.declarativeUiPath.has_value()
        || (profile == ComponentActivationManifestProfileV1::BoundedAudioStream
            && (manifest.minimumEngineApiVersion
                    != TALAS_KERNEL_API_VERSION_V3
                || manifest.maximumEngineApiVersion
                    != TALAS_KERNEL_API_VERSION_V3))
        || (profile
                == ComponentActivationManifestProfileV1::
                    MultipleImmutableResources
            && (manifest.minimumEngineApiVersion
                    != TALAS_KERNEL_API_VERSION_V4
                || manifest.maximumEngineApiVersion
                    != TALAS_KERNEL_API_VERSION_V4))
        || opaqueStateByteCount != 0)
    {
        return ComponentActivationManifestProfileV1::Unsupported;
    }
    const auto& node = manifest.nodes.front();
    if (node.stateSchemaVersion != manifest.state.currentSchemaVersion
        || !componentActivationSupportsChannelCountV1(inputChannelCount)
        || !componentActivationSupportsChannelCountV1(outputChannelCount)
        || node.inputs.size() != inputChannelCount
        || node.outputs.size() != outputChannelCount
        || node.parameters.size()
            > maximumComponentActivationStaticParametersV1)
    {
        return ComponentActivationManifestProfileV1::Unsupported;
    }
    const auto audioPort = [](const auto& port) {
        return port.type == talas::core::PortType::Audio
            && port.signal.timelineBearing;
    };
    if (!std::ranges::all_of(node.inputs, audioPort)
        || !std::ranges::all_of(node.outputs, audioPort)
        || !std::ranges::all_of(
            node.parameters,
            [](const auto& parameter) {
                return knownParameterKind(parameter.kind);
            }))
    {
        return ComponentActivationManifestProfileV1::Unsupported;
    }
    if ((profile == ComponentActivationManifestProfileV1::StatelessAudio
            && resourceCount != 0)
        || ((profile
                == ComponentActivationManifestProfileV1::ImmutableResource
             || profile
                == ComponentActivationManifestProfileV1::BoundedAudioStream)
            && resourceCount != 1)
        || (profile
                == ComponentActivationManifestProfileV1::
                    MultipleImmutableResources
            && (resourceCount < 2
                || resourceCount
                    > maximumComponentActivationResourceSetMembersV1)))
    {
        return ComponentActivationManifestProfileV1::Unsupported;
    }
    return profile;
}

bool componentActivationManifestClaimsMatchRuntimeProfileV1(
    const component_bundle::ComponentBundleSnapshot& snapshot,
    ComponentActivationMacOSProfileV1 profile) noexcept
{
    const auto realtimeRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    const auto resourceRoles = realtimeRoles
        | componentRole(ComponentExecutionRole::ImmutableResourceConsumer);
    const auto audioRoles = realtimeRoles
        | componentRole(ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto combinedRoles = resourceRoles
        | componentRole(ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto realtimePermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    const auto resourcePermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioPermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto combinedPermissions = resourcePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);

    auto expectedRoles = std::uint64_t{0};
    auto expectedPermissions = std::uint64_t{0};
    switch (profile)
    {
        case ComponentActivationMacOSProfileV1::StatelessAudio:
            expectedRoles = realtimeRoles;
            expectedPermissions = realtimePermissions;
            break;
        case ComponentActivationMacOSProfileV1::ImmutableResource:
        case ComponentActivationMacOSProfileV1::
            MultipleImmutableOnlyResources:
            expectedRoles = resourceRoles;
            expectedPermissions = resourcePermissions;
            break;
        case ComponentActivationMacOSProfileV1::BoundedAudioStream:
            expectedRoles = audioRoles;
            expectedPermissions = audioPermissions;
            break;
        case ComponentActivationMacOSProfileV1::MultipleImmutableResources:
            expectedRoles = combinedRoles;
            expectedPermissions = combinedPermissions;
            break;
    }
    return snapshot.manifest.declaredRoles == expectedRoles
        && snapshot.manifest.requestedPermissions == expectedPermissions;
}
}
