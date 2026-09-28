#include "binding_internal.hpp"
#include "refl/container_adapter.hpp"

#include <lua.h>
#include <lualib.h>
#include <memory>
#include <utility>

namespace ets::detail {

LuauObjectView check_luau_object(lua_State* state, int index) {
    const auto tag = static_cast<LuauObjectTag>(lua_userdatatag(state, index));
    void* userdata = lua_touserdata(state, index);
    switch (tag) {
        case LuauObjectTag::BorrowedRead: {
            auto* object = static_cast<LuauBorrowedObject*>(userdata);
            if (!luau_borrow_is_valid(object->scope, object->token)) {
                luaL_error(state, "attempt to access an expired ECS borrow");
            }
            return {
                .ref = object->ref,
                .scope = object->scope,
                .token = object->token,
            };
        }
        case LuauObjectTag::BorrowedWrite: {
            auto* object = static_cast<LuauMutableBorrowedObject*>(userdata);
            if (!luau_borrow_is_valid(object->scope, object->token)) {
                luaL_error(state, "attempt to access an expired ECS borrow");
            }
            return {
                .ref = object->ref,
                .scope = object->scope,
                .token = object->token,
                .mutation = object->mutation,
            };
        }
        case LuauObjectTag::Owned: {
            auto* object = static_cast<LuauOwnedObject*>(userdata);
            return {
                .ref = object->ref,
                .owner = &object->owner,
            };
        }
    }
    luaL_typeerror(state, index, "reflected value");
}

namespace {

void push_owned_object(lua_State* state, Ref ref, std::shared_ptr<Val> owner) {
    auto* object = new (lua_newuserdatataggedwithmetatable(
        state,
        sizeof(LuauOwnedObject),
        static_cast<int>(LuauObjectTag::Owned)
    )) LuauOwnedObject {
        .ref = ref,
        .owner = std::move(owner),
    };
    static_cast<void>(object);
}

} // namespace

void push_luau_readonly_value(lua_State* state, Val value) {
    auto owner = std::make_shared<Val>(std::move(value));
    push_luau_ref(
        state,
        std::as_const(*owner).ref(),
        LuauObjectView {.owner = &owner}
    );
}

void push_luau_ref(lua_State* state, Ref ref, const LuauObjectView& parent) {
    if (!ref) {
        lua_pushnil(state);
        return;
    }
    if (push_luau_primitive(state, ref)) {
        return;
    }
    auto adapter =
        Registry::instance().try_get_container_adapter(ref.type_id());
    if (adapter && adapter->kind() == ContainerKind::Optional) {
        auto* indexed = adapter->indexed();
        auto size = adapter->size(ref);
        if (!indexed || !size) {
            luaL_error(state, "Invalid reflected optional value");
            return;
        }
        if (*size == 0) {
            lua_pushnil(state);
            return;
        }
        auto value = indexed->at(ref, 0);
        if (!value) {
            luaL_error(state, "%s", value.error().message.c_str());
            return;
        }
        push_luau_ref(state, *value, parent);
        return;
    }
    if (parent.owner != nullptr) {
        push_owned_object(state, ref, *parent.owner);
    } else if (parent.scope != nullptr) {
        push_luau_borrowed_object(
            state,
            ref,
            *parent.scope,
            parent.token,
            parent.mutation
        );
    } else {
        push_owned_object(state, ref, {});
    }
}

} // namespace ets::detail
