#include "scripting/detail/binding.hpp"

#include "binding_internal.hpp"
#include "scripting/detail/commands_binding.hpp"
#include "scripting/detail/world_binding.hpp"

#include <array>
#include <lua.h>
#include <lualib.h>

namespace ets::detail {
namespace {

constexpr const char* c_borrowed_metatable = "ets.borrowed";

void destroy_owned_object(lua_State*, void* userdata) {
    static_cast<LuauOwnedObject*>(userdata)->~LuauOwnedObject();
}

int borrowed_call(lua_State* state) {
    auto object = check_luau_object(state, 1);
    if (is_luau_dynamic_param_callable(object.ref)) {
        return invoke_luau_dynamic_param(state);
    }
    luaL_error(state, "value is not callable");
}

} // namespace

void install_luau_borrowed_object_metatable(lua_State* state) {
    if (luaL_newmetatable(state, c_borrowed_metatable)) {
        const int metatable = lua_gettop(state);
        install_luau_property_metatable(state, metatable);
        lua_pushcfunction(state, borrowed_call, "borrowed.__call");
        lua_setfield(state, metatable, "__call");
        lua_pushcfunction(state, luau_borrowed_iter, "borrowed.__iter");
        lua_setfield(state, metatable, "__iter");
        lua_pushcfunction(state, luau_container_length, "container.__len");
        lua_setfield(state, metatable, "__len");

        constexpr std::array object_tags {
            LuauObjectTag::BorrowedRead,
            LuauObjectTag::BorrowedWrite,
            LuauObjectTag::Owned,
        };
        for (const auto tag : object_tags) {
            lua_pushvalue(state, metatable);
            lua_setuserdatametatable(state, static_cast<int>(tag));
        }
        lua_setuserdatadtor(
            state,
            static_cast<int>(LuauObjectTag::Owned),
            destroy_owned_object
        );
        install_luau_property_direct_access(state);
    }
    lua_pop(state, 1);

    if (luaL_newmetatable(state, c_luau_type_token_metatable)) {
        lua_pushcfunction(state, luau_type_token_index, "type.__index");
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, luau_type_token_call, "type.__call");
        lua_setfield(state, -2, "__call");
        lua_pushstring(state, "protected type token");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);
    install_luau_commands_metatables(state);
    install_luau_world_metatables(state);
}

LuauBorrowedRef check_luau_borrowed_ref(lua_State* state, int index) {
    auto object = check_luau_object(state, index);
    return {
        .ref = object.ref,
        .scope = object.scope,
        .token = object.token,
        .mutation = object.mutation,
    };
}

void push_luau_borrowed_ref(
    lua_State* state,
    Ref ref,
    LuauBorrowScope& scope,
    LuauBorrowToken token,
    LuauMutationContext mutation
) {
    push_luau_borrowed_value(state, ref, scope, token, mutation);
}

} // namespace ets::detail
