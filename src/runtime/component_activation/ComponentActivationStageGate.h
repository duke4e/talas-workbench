#pragma once

#include <shared_mutex>

namespace talas::runtime::detail
{
struct ComponentActivationStageGateV1
{
    mutable std::shared_mutex mutex;
};
}
