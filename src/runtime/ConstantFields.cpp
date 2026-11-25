#include "talas/runtime/ConstantFields.h"

#include "StartOnlyFields.h"

namespace talas::runtime
{
bool isConstantField(std::string_view typeId, std::string_view parameterId) noexcept
{
    return start_only_fields::contains(typeId, parameterId);
}
}
