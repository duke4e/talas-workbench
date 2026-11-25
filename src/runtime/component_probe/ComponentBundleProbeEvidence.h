#pragma once

#include "talas/runtime/ComponentTrustPolicy.h"
#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <memory>
#include <optional>

namespace talas::runtime::component_probe
{
class ComponentBundleProbeValidationSessionV1
{
public:
    ComponentBundleProbeValidationSessionV1() noexcept = default;
    ~ComponentBundleProbeValidationSessionV1();

    ComponentBundleProbeValidationSessionV1(
        ComponentBundleProbeValidationSessionV1&&) noexcept;
    ComponentBundleProbeValidationSessionV1& operator=(
        ComponentBundleProbeValidationSessionV1&&) noexcept;

    ComponentBundleProbeValidationSessionV1(
        const ComponentBundleProbeValidationSessionV1&) = delete;
    ComponentBundleProbeValidationSessionV1& operator=(
        const ComponentBundleProbeValidationSessionV1&) = delete;

    void revoke() noexcept;

private:
    explicit ComponentBundleProbeValidationSessionV1(
        std::shared_ptr<detail::ComponentProbeValidationSessionStateV1> state)
        noexcept;

    std::shared_ptr<detail::ComponentProbeValidationSessionStateV1> state_;

    friend class ComponentBundleProbeEvidenceFactory;
};

struct ComponentBundleProbeEvidenceAuthoringV1
{
    ComponentBundleProbeValidationSessionV1 session;
    ComponentProbeEvidence evidence;
};

class ComponentBundleProbeEvidenceFactory
{
public:
    [[nodiscard]] static std::optional<
        ComponentBundleProbeEvidenceAuthoringV1>
    makeFromValidatedResult(
        const ComponentBundleProbeResultV1& result,
        const ComponentBundleProbeRequestV1& request) noexcept;

    [[nodiscard]] static std::optional<
        ComponentBundleProbeEvidenceAuthoringV1>
    makeParentClassifiedFailure(
        ComponentProbeState state,
        const ComponentBundleProbeRequestV1& request) noexcept;

private:
    [[nodiscard]] static std::optional<
        ComponentBundleProbeEvidenceAuthoringV1>
    author(ComponentProbeEvidenceView view) noexcept;
};
}
