#include "binding_internal.hpp"
#include "refl/container_adapter.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace ets::detail {
namespace {
auto adapter_for(const LuauObjectView& object) {
    return Registry::instance().try_get_container_adapter(object.ref.type_id());
}

void push_element(lua_State* state, Ref value, const LuauObjectView& parent) {
    if (parent.ref.is_const()) {
        value = Ref(value.const_ptr(), value.type_id());
    }
    push_luau_ref(state, value, parent);
}

int lookup(
    lua_State* state,
    const LuauObjectView& object,
    const ContainerAdapter& adapter,
    int key
) {
    if (const auto* indexed = adapter.indexed()) {
        if (lua_type(state, key) != LUA_TNUMBER) {
            throw std::invalid_argument("Expected an integer container index");
        }
        const double index = lua_tonumber(state, key);
        if (!std::isfinite(index) || std::trunc(index) != index) {
            throw std::invalid_argument("Expected an integer container index");
        }
        const auto size = adapter.size(object.ref);
        if (!size) {
            throw std::runtime_error(size.error().message);
        }
        if (index < 1 || index > static_cast<double>(*size)) {
            lua_pushnil(state);
            return 1;
        }
        auto element =
            indexed->at(object.ref, static_cast<std::size_t>(index - 1));
        if (!element) {
            throw std::runtime_error(element.error().message);
        }
        push_element(state, *element, object);
        return 1;
    }
    const auto* associative = adapter.associative();
    if (!associative) {
        throw std::invalid_argument("Container does not support indexing");
    }
    const auto key_type = associative->key_type(object.ref);
    if (!key_type) {
        throw std::runtime_error(key_type.error().message);
    }
    auto value = luau_value_for_type(state, key, *key_type);
    if (!value) {
        throw std::invalid_argument(value.error());
    }
    if (adapter.kind() == ContainerKind::Set) {
        auto found = associative->contains(object.ref, value->ref());
        if (!found) {
            throw std::runtime_error(found.error().message);
        }
        lua_pushboolean(state, *found);
        return 1;
    }
    auto found = associative->find(object.ref, value->ref());
    if (!found && found.error().kind == ContainerError::Kind::NotFound) {
        lua_pushnil(state);
    } else if (!found) {
        throw std::runtime_error(found.error().message);
    } else {
        push_element(state, *found, object);
    }
    return 1;
}

int container_next(lua_State* state) {
    try {
        auto object = check_luau_object(state, lua_upvalueindex(1));
        auto adapter = adapter_for(object);
        if (!adapter) {
            throw std::runtime_error("Container adapter is unavailable");
        }
        const int index = lua_tointeger(state, lua_upvalueindex(2)) + 1;
        lua_pushinteger(state, index);
        lua_replace(state, lua_upvalueindex(2));
        if (adapter->indexed()) {
            auto size = adapter->size(object.ref);
            if (!size) {
                throw std::runtime_error(size.error().message);
            }
            if (std::cmp_greater(index, *size)) {
                return 0;
            }
            lua_pushinteger(state, index);
        } else {
            lua_rawgeti(state, lua_upvalueindex(3), index);
            if (lua_isnil(state, -1)) {
                return 0;
            }
        }
        lookup(state, object, *adapter, -1);
        return 2;
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    lua_error(state);
}
} // namespace

bool push_luau_container_index(lua_State* state, const LuauObjectView& object) {
    try {
        auto adapter = adapter_for(object);
        if (!adapter) {
            return false;
        }
        lookup(state, object, *adapter, 2);
        return true;
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    lua_error(state);
}

int luau_container_length(lua_State* state) {
    try {
        auto object = check_luau_object(state, 1);
        auto adapter = adapter_for(object);
        if (!adapter) {
            throw std::invalid_argument("Value is not a reflected container");
        }
        auto size = adapter->size(object.ref);
        if (!size) {
            throw std::runtime_error(size.error().message);
        }
        lua_pushnumber(state, static_cast<double>(*size));
        return 1;
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    lua_error(state);
}

bool push_luau_container_iterator(
    lua_State* state,
    const LuauObjectView& object
) {
    try {
        auto adapter = adapter_for(object);
        if (!adapter) {
            return false;
        }
        // Retain the original userdata, so iteration checks borrow validity and
        // keeps owned containers alive. Associative iteration snapshots keys.
        lua_pushvalue(state, 1);
        lua_pushinteger(state, 0);
        lua_newtable(state);
        if (const auto* associative = adapter->associative()) {
            const int keys = lua_gettop(state);
            auto result = associative->for_each_entry(
                object.ref,
                [&](AssociativeElementRef entry,
                    std::size_t index) -> Status<ContainerError> {
                    push_element(state, entry.key, object);
                    lua_rawseti(state, keys, static_cast<int>(index + 1));
                    return {};
                }
            );
            if (!result) {
                throw std::runtime_error(result.error().message);
            }
        }
        lua_pushcclosure(state, container_next, "container.next", 3);
        return true;
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    lua_error(state);
}
} // namespace ets::detail
