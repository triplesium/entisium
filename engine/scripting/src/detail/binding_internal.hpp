#pragma once

#include "ecs/fwd.hpp"
#include "refl/registry.hpp"
#include "scripting/detail/binding.hpp"

#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace ets::detail {

inline constexpr const char* c_luau_type_token_metatable = "ets.type";

enum class LuauObjectTag : int {
    BorrowedRead = 1,
    BorrowedWrite = 2,
    Owned = 3,
};

struct LuauBorrowedObject {
    Ref ref;
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
};

struct LuauMutableBorrowedObject {
    Ref ref;
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
    LuauMutationContext mutation;
};

struct LuauOwnedObject {
    Ref ref;
    std::shared_ptr<Val> owner;
};

struct LuauObjectView {
    Ref ref;
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
    LuauMutationContext mutation;
    const std::shared_ptr<Val>* owner {nullptr};
};

bool push_luau_container_index(lua_State*, const LuauObjectView&);
bool push_luau_container_iterator(lua_State*, const LuauObjectView&);
int luau_container_length(lua_State*);

struct LuauTypeToken {
    TypeId type;
};

int luau_type_token_call(lua_State* state);

static_assert(std::is_trivially_destructible_v<LuauBorrowedObject>);
static_assert(std::is_trivially_destructible_v<LuauMutableBorrowedObject>);

inline bool
luau_borrow_is_valid(const LuauBorrowScope* scope, LuauBorrowToken token) {
    return scope != nullptr && scope->valid(token);
}

inline bool push_luau_primitive(lua_State* state, Ref ref) {
    const TypeId id = ref.type_id();
    if (id == type_id<TypeId>()) {
        push_luau_type_token(state, ref.get_const<TypeId>());
    } else if (id == type_id<Entity>()) {
        lua_pushunsigned(state, ref.get_const<Entity>().value);
    } else if (id == type_id<bool>()) {
        lua_pushboolean(state, ref.get_const<bool>());
    } else if (id == type_id<std::string>()) {
        const auto& value = ref.get_const<std::string>();
        lua_pushlstring(state, value.data(), value.size());
    } else if (id == type_id<std::string_view>()) {
        const auto value = ref.get_const<std::string_view>();
        lua_pushlstring(state, value.data(), value.size());
    } else {
        auto type = Registry::instance().try_get_type(id);
        if (!type) {
            return false;
        }
        if (type->is_integral()) {
            const auto value = ref.to_number<long double>();
            if (value < -9007199254740991.0L || value > 9007199254740991.0L) {
                luaL_error(state, "Integer outside exact Luau range");
            }
            lua_pushnumber(state, static_cast<double>(value));
        } else if (type->is_floating_point()) {
            const auto value = ref.to_number<double>();
            if (!std::isfinite(value)) {
                luaL_error(state, "Non-finite result");
            }
            lua_pushnumber(state, value);
        } else {
            return false;
        }
    }
    return true;
}

inline void push_luau_borrowed_object(
    lua_State* state,
    Ref ref,
    LuauBorrowScope& scope,
    LuauBorrowToken token,
    LuauMutationContext mutation = {}
) {
    if (!ref.is_const()) {
        auto* object = new (lua_newuserdatataggedwithmetatable(
            state,
            sizeof(LuauMutableBorrowedObject),
            static_cast<int>(LuauObjectTag::BorrowedWrite)
        )) LuauMutableBorrowedObject {
            .ref = ref,
            .scope = &scope,
            .token = token,
            .mutation = mutation,
        };
        static_cast<void>(object);
    } else {
        auto* object = new (lua_newuserdatataggedwithmetatable(
            state,
            sizeof(LuauBorrowedObject),
            static_cast<int>(LuauObjectTag::BorrowedRead)
        )) LuauBorrowedObject {
            .ref = ref,
            .scope = &scope,
            .token = token,
        };
        static_cast<void>(object);
    }
}

inline void push_luau_borrowed_value(
    lua_State* state,
    Ref ref,
    LuauBorrowScope& scope,
    LuauBorrowToken token,
    LuauMutationContext mutation = {}
) {
    if (!ref) {
        lua_pushnil(state);
        return;
    }
    if (push_luau_primitive(state, ref)) {
        return;
    }
    push_luau_borrowed_object(state, ref, scope, token, mutation);
}

LuauObjectView check_luau_object(lua_State* state, int index);
void push_luau_ref(lua_State* state, Ref ref, const LuauObjectView& parent);
int luau_borrowed_iter(lua_State* state);
bool push_luau_dynamic_param_member(
    lua_State* state,
    TypeId type,
    std::string_view key
);
bool is_luau_dynamic_param_callable(Ref receiver);
int invoke_luau_dynamic_param(lua_State* state);
bool luau_reflected_values_equal(Ref lhs, Ref rhs);
Result<Val, std::string>
luau_value_for_type(lua_State* state, int index, TypeId expected);
Result<Val, std::string>
luau_value_for_property(lua_State* state, int index, TypeId expected);
int luau_type_token_index(lua_State* state);
int luau_invoke_method(lua_State* state);
void install_luau_property_metatable(lua_State* state, int metatable);
void install_luau_property_direct_access(lua_State* state);

} // namespace ets::detail
