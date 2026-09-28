#pragma once
#include "refl/type.hpp"

#include <string>

namespace ets {
// A process-independent description; never transmits addresses or local IDs.
std::string luau_type_descriptor(TypeId type);
} // namespace ets
