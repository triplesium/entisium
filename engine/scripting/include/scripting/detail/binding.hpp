#pragma once

#include "base/result.hpp"
#include "ecs/change_detection.hpp"
#include "refl/ref.hpp"
#include "refl/val.hpp"
#include "scripting/detail/borrow_scope.hpp"

#include <span>
#include <string>
#include <string_view>

struct lua_State;

namespace ets {
class Method;

struct LuauPropertyPathDecl;

namespace detail {
int invoke_luau_library(
    lua_State* state,
    const Method& method,
    const std::shared_ptr<Val>& owner
);

struct LuauMutationContext {
    ComponentTicks* ticks {nullptr};
    Tick tick {0};

    explicit operator bool() const { return ticks != nullptr; }

    void mark_changed() const {
        if (ticks != nullptr) {
            ticks->mark_changed(tick);
        }
    }
};

struct LuauBorrowedRef {
    Ref ref;
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
    LuauMutationContext mutation;
};

void install_luau_borrowed_object_metatable(lua_State* state);
Status<std::string> register_luau_property_paths(
    lua_State* state,
    std::span<const LuauPropertyPathDecl> paths
);
Status<std::string> refresh_luau_property_paths(lua_State* state);
int luau_reusable_query(lua_State* state);
int luau_chunk_query(lua_State* state);
LuauBorrowedRef check_luau_borrowed_ref(lua_State* state, int index);
Result<Val, std::string> copy_luau_reflected_value(
    lua_State* state,
    int index,
    std::string_view context
);
void push_luau_owned_value(lua_State* state, Val value);
void push_luau_readonly_value(lua_State* state, Val value);
void push_luau_type_token(lua_State* state, TypeId type);
TypeId
check_luau_type_token(lua_State* state, int index, std::string_view context);
void push_luau_borrowed_ref(
    lua_State* state,
    Ref ref,
    LuauBorrowScope& scope,
    LuauBorrowToken token,
    LuauMutationContext mutation = {}
);

} // namespace detail
} // namespace ets
