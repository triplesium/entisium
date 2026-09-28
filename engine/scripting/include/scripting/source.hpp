#pragma once

#include <string>

namespace ets {

struct LuauScriptSource {
    std::string name;
    std::string content;
    // Game modules prepare reflected types; ordinary libraries only execute
    // Luau.
    bool runtime_types {true};
};

} // namespace ets
