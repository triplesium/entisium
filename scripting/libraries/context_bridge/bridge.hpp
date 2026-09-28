#pragma once
#include "refl/val.hpp"
#include "scripting/annotations.hpp" // IWYU pragma: keep
#include "scripting/library.hpp"

#include <nlohmann/json.hpp> // IWYU pragma: keep
namespace ets::luau::context_bridge {
ETS_REFLECT(LuauLibrary(name = "@context/bridge"))
class Library final {
  public:
    explicit Library(LuauLibraryServices& services);
    ETS_REFLECT(LuauExport(signature = "<T>(TypeToken<T>) -> any"))
    static nlohmann::json type_descriptor(TypeId type);
    ETS_REFLECT(LuauExport(signature = "<T>(TypeToken<T>, string) -> T"))
    static const Val decode_snapshot(TypeId type, std::string text);
    ETS_REFLECT(LuauExport(signature = "(unknown) -> string"))
    static std::string encode(nlohmann::json value);
    ETS_REFLECT(LuauExport(signature = "<T>(T) -> T"))
    static const Val copy_value(Val value);
};
} // namespace ets::luau::context_bridge
