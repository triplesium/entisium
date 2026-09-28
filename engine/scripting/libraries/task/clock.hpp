#pragma once
#include "scripting/annotations.hpp" // IWYU pragma: keep

namespace ets::luau::task {
ETS_REFLECT(LuauLibrary(name = "@task"))
struct Library {
    static double now();
};
} // namespace ets::luau::task
