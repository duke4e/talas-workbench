#pragma once

#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleReplacementPolicy.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace talas::runtime::detail
{
struct ComponentBundleReplacementConfirmationStateV1;
}

namespace talas::runtime::component_bundle
{
inline constexpr std::size_t componentBundleReplacementSessionNonceBytes = 32;

struct ComponentBundleReplacementConfirmationTestPeer;

enum class ComponentBundleReplacementPrepareCode : std::uint8_t
{
    Ready,
    Incompatible,
    InvalidInput,
    Busy,
    AllocationFailed,
    GenerationExhausted,
};

struct ComponentBundleReplacementPrepareInputs
{
    std::uint64_t documentGeneration = 0;
    std::string_view nodeId;
    ComponentBundleReplacementRequest replacement;
};

struct ComponentBundleReplacementPrepareResult
{
    ComponentBundleReplacementPrepareCode code =
        ComponentBundleReplacementPrepareCode::InvalidInput;
    std::uint64_t previewGeneration = 0;
    std::shared_ptr<const ComponentBundleReplacementPreview> preview;

    [[nodiscard]] bool ready() const noexcept
    {
        return code == ComponentBundleReplacementPrepareCode::Ready
            && previewGeneration != 0 && preview != nullptr
            && preview->compatible();
    }
};

struct ComponentBundleReplacementConsumptionExpectation
{
    std::uint64_t documentGeneration = 0;
    std::string_view nodeId;
    std::array<std::uint8_t, componentTrustDigestBytes>
        preservedCapsuleDigest{};
    ComponentApprovalFingerprint candidate;
    std::span<const ComponentBundleReplacementResourceTransition>
        resourceTransitions;
};

class ComponentBundleReplacementConsumptionLease
{
public:
    ComponentBundleReplacementConsumptionLease() noexcept = default;
    ~ComponentBundleReplacementConsumptionLease();

    ComponentBundleReplacementConsumptionLease(
        ComponentBundleReplacementConsumptionLease&&) noexcept;
    ComponentBundleReplacementConsumptionLease& operator=(
        ComponentBundleReplacementConsumptionLease&&) noexcept;

    ComponentBundleReplacementConsumptionLease(
        const ComponentBundleReplacementConsumptionLease&) = delete;
    ComponentBundleReplacementConsumptionLease& operator=(
        const ComponentBundleReplacementConsumptionLease&) = delete;

    [[nodiscard]] bool approved() const noexcept;
    void commit() noexcept;

private:
    ComponentBundleReplacementConsumptionLease(
        std::shared_ptr<
            detail::ComponentBundleReplacementConfirmationStateV1> state,
        std::unique_lock<std::mutex> stateLock,
        ComponentOwnerAuthorizationLeaseV1 ownerLease,
        std::uint64_t previewGeneration) noexcept;

    void release(bool consumed) noexcept;

    std::shared_ptr<detail::ComponentBundleReplacementConfirmationStateV1>
        state_;
    std::unique_lock<std::mutex> stateLock_;
    ComponentOwnerAuthorizationLeaseV1 ownerLease_;
    std::uint64_t previewGeneration_ = 0;
    bool committed_ = false;

    friend class ComponentBundleReplacementReceipt;
};

class ComponentBundleReplacementReceipt
{
public:
    ComponentBundleReplacementReceipt() noexcept = default;
    ~ComponentBundleReplacementReceipt();

    ComponentBundleReplacementReceipt(
        ComponentBundleReplacementReceipt&&) noexcept;
    ComponentBundleReplacementReceipt& operator=(
        ComponentBundleReplacementReceipt&&) noexcept;

    ComponentBundleReplacementReceipt(
        const ComponentBundleReplacementReceipt&) = delete;
    ComponentBundleReplacementReceipt& operator=(
        const ComponentBundleReplacementReceipt&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::uint64_t previewGeneration() const noexcept
    {
        return previewGeneration_;
    }
    [[nodiscard]] ComponentBundleReplacementConsumptionLease tryConsume(
        const ComponentBundleReplacementConsumptionExpectation& expectation)
        noexcept;
    void cancel() noexcept;

private:
    ComponentBundleReplacementReceipt(
        std::shared_ptr<
            detail::ComponentBundleReplacementConfirmationStateV1> state,
        std::array<
            std::uint8_t,
            componentBundleReplacementSessionNonceBytes> sessionNonce,
        std::uint64_t previewGeneration) noexcept;

    std::shared_ptr<detail::ComponentBundleReplacementConfirmationStateV1>
        state_;
    std::array<std::uint8_t, componentBundleReplacementSessionNonceBytes>
        sessionNonce_{};
    std::uint64_t previewGeneration_ = 0;

    friend class ComponentBundleReplacementConfirmationStore;
    friend struct ComponentBundleReplacementConfirmationTestPeer;
};

class ComponentBundleReplacementConfirmationStore
{
public:
    explicit ComponentBundleReplacementConfirmationStore(
        std::array<
            std::uint8_t,
            componentBundleReplacementSessionNonceBytes> sessionNonce);
    ~ComponentBundleReplacementConfirmationStore();

    ComponentBundleReplacementConfirmationStore(
        const ComponentBundleReplacementConfirmationStore&) = delete;
    ComponentBundleReplacementConfirmationStore& operator=(
        const ComponentBundleReplacementConfirmationStore&) = delete;

    [[nodiscard]] ComponentBundleReplacementPrepareResult prepare(
        const ComponentBundleReplacementPrepareInputs& inputs) noexcept;
    [[nodiscard]] std::optional<ComponentBundleReplacementReceipt> confirm(
        std::uint64_t previewGeneration) noexcept;
    [[nodiscard]] bool cancel(std::uint64_t previewGeneration) noexcept;
    [[nodiscard]] std::shared_ptr<const ComponentBundleReplacementPreview>
    preview(std::uint64_t previewGeneration) const noexcept;
private:
    std::shared_ptr<detail::ComponentBundleReplacementConfirmationStateV1>
        state_;

    friend struct ComponentBundleReplacementConfirmationTestPeer;
};

struct ComponentBundleReplacementConfirmationTestPeer
{
    [[nodiscard]] static bool cancelReceiptWhileStoreLocked(
        ComponentBundleReplacementConfirmationStore& store,
        ComponentBundleReplacementReceipt& receipt) noexcept;
};
}
