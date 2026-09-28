#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string_view>
struct lua_State;
namespace ets::detail {
nlohmann::json luau_json(
    lua_State*,
    int,
    std::string_view,
    std::size_t,
    std::size_t&,
    bool reflected = false
);
nlohmann::json luau_json(lua_State*, int, std::string_view);
void push_json(
    lua_State*,
    const nlohmann::json&,
    std::size_t depth = 0,
    bool allow_null = false
);
} // namespace ets::detail
