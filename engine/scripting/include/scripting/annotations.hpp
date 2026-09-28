#pragma once

#include "refl/reflect.hpp"

#include <string>

namespace ets::annotations {

ETS_ANNOTATION(LuauLibrary)
struct LuauLibrary {
    std::string name;
    // Custom adapters implement LuauNativeLibrary; metadata describes their
    // exports.
    bool custom {false};
    std::string globals;
    std::string types;
    std::string exports;
};

ETS_ANNOTATION(LuauExport)
struct LuauExport {
    std::string name;
    std::string signature;
};

ETS_ANNOTATION(LuauType)
struct LuauType {
    std::string name;
    std::string input;
};

} // namespace ets::annotations
