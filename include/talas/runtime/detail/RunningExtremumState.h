#pragma once

namespace talas::runtime::detail
{
// Internal value storage shared by offline continuation and realtime journals.
// This record is not an execution authority or a public kernel ABI contract.
struct RunningExtremumState
{
    bool initialized = false;
    double extremum = 0.0;
    bool operator==(const RunningExtremumState&) const = default;
};
}
