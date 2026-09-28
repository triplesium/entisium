#pragma once
#include "scripting/annotations.hpp" // IWYU pragma: keep
#include "scripting/library.hpp"
namespace ets::luau::json {
ETS_REFLECT(LuauLibrary(
    name = "@json",
    custom = true,
    globals = "declare extern type __ets_JsonNull with\nend",
    types = "export type Null = __ets_JsonNull",
    exports = "read null: Null, read encode: (unknown) -> string, read decode: "
              "(string) -> unknown, read array: <T>({T}) -> {T}"
))
class Library final : public LuauNativeLibrary {
  public:
    explicit Library(LuauLibraryServices&) {}
    void open(lua_State* state) override;
};
} // namespace ets::luau::json
