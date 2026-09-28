#include "scripting/detail/json_value.hpp"

#include "scripting/detail/binding.hpp"
#include "serialization/json_archive.hpp"
#include "serialization/serializer.hpp"

#include <cmath>
#include <limits>
#include <lua.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace ets::detail {
using Json = nlohmann::json;
constexpr std::size_t c_max_playtest_json_depth = 32;
constexpr std::size_t c_max_playtest_json_nodes = std::size_t {16} * 1024;

int absolute_index(lua_State* state, int index) {
    return index > 0 || index <= LUA_REGISTRYINDEX ?
               index :
               lua_gettop(state) + index + 1;
}

Json luau_json(
    lua_State* state,
    int index,
    std::string_view context,
    std::size_t depth,
    std::size_t& nodes,
    bool reflected
) {
    if (depth > c_max_playtest_json_depth ||
        ++nodes > c_max_playtest_json_nodes) {
        throw std::runtime_error(
            std::string(context) + " exceeds the JSON complexity limit"
        );
    }
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            if (reflected) {
                return nullptr;
            }
            throw std::runtime_error(std::string(context) + " contains nil");
        case LUA_TUSERDATA:
            if (reflected) {
                const auto object =
                    detail::copy_luau_reflected_value(state, index, context);
                if (!object) {
                    throw std::runtime_error(object.error());
                }
                auto node = serialization::serialize(
                    object->ref(),
                    {.include_type_tag = false}
                );
                if (!node) {
                    throw std::runtime_error(
                        std::string(context) + ": " + node.error().message
                    );
                }
                auto text = serialization::write_json(*node, -1);
                if (!text) {
                    throw std::runtime_error(text.error().message);
                }
                return Json::parse(*text);
            }
            throw std::runtime_error(
                std::string(context) + " contains unsupported userdata"
            );
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) != 0;
        case LUA_TNUMBER: {
            const auto value = lua_tonumber(state, index);
            if (!std::isfinite(value)) {
                throw std::runtime_error(
                    std::string(context) + " contains a non-finite number"
                );
            }
            if (std::trunc(value) == value &&
                value >= static_cast<double>(
                             std::numeric_limits<std::int64_t>::min()
                         ) &&
                value <= static_cast<double>(
                             std::numeric_limits<std::int64_t>::max()
                         )) {
                return static_cast<std::int64_t>(value);
            }
            return value;
        }
        case LUA_TSTRING: {
            std::size_t size = 0;
            const char* value = lua_tolstring(state, index, &size);
            return std::string(value, size);
        }
        case LUA_TTABLE:
            break;
        default:
            throw std::runtime_error(
                std::string(context) +
                " must contain only booleans, numbers, strings, and tables"
            );
    }

    const int table = absolute_index(state, index);
    const auto array_size = static_cast<std::size_t>(lua_objlen(state, table));
    std::size_t entries = 0;
    bool string_keys = false;
    bool number_keys = false;
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        ++entries;
        if (lua_type(state, -2) == LUA_TSTRING) {
            string_keys = true;
        } else if (lua_type(state, -2) == LUA_TNUMBER) {
            const auto key = lua_tonumber(state, -2);
            if (std::trunc(key) != key || key < 1.0 ||
                key > static_cast<double>(array_size)) {
                lua_pop(state, 1);
                throw std::runtime_error(
                    std::string(context) + " contains an invalid array key"
                );
            }
            number_keys = true;
        } else {
            lua_pop(state, 1);
            throw std::runtime_error(
                std::string(context) + " contains an unsupported table key"
            );
        }
        lua_pop(state, 1);
    }
    if (array_size > 0) {
        if (string_keys || !number_keys || entries != array_size) {
            throw std::runtime_error(
                std::string(context) +
                " cannot mix array entries with object fields"
            );
        }
        Json result = Json::array();
        for (std::size_t item = 1; item <= array_size; ++item) {
            lua_rawgeti(state, table, static_cast<int>(item));
            result.push_back(luau_json(
                state,
                -1,
                std::string(context) + "[" + std::to_string(item) + "]",
                depth + 1,
                nodes,
                reflected
            ));
            lua_pop(state, 1);
        }
        return result;
    }
    if (number_keys) {
        throw std::runtime_error(
            std::string(context) + " contains a sparse array"
        );
    }
    Json result = Json::object();
    if (reflected && entries == 0 && lua_getmetatable(state, table)) {
        lua_getfield(state, -1, "__context_array");
        const bool array = lua_toboolean(state, -1) != 0;
        lua_pop(state, 2);
        if (array) {
            return Json::array();
        }
    }
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        std::size_t key_size = 0;
        const char* key = lua_tolstring(state, -2, &key_size);
        result[std::string(key, key_size)] = luau_json(
            state,
            -1,
            std::string(context) + "." + std::string(key, key_size),
            depth + 1,
            nodes,
            reflected
        );
        lua_pop(state, 1);
    }
    return result;
}

Json luau_json(lua_State* state, int index, std::string_view context) {
    std::size_t nodes = 0;
    return luau_json(state, index, context, 0, nodes);
}

void push_json(
    lua_State* state,
    const Json& value,
    std::size_t depth,
    bool allow_null
) {
    if (depth > c_max_playtest_json_depth) {
        throw std::runtime_error(
            "Playtest action exceeds the JSON depth limit"
        );
    }
    if (value.is_null()) {
        if (allow_null) {
            lua_pushnil(state);
            return;
        }
        throw std::runtime_error("Playtest actions do not support null");
    }
    if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>());
    } else if (value.is_number_integer()) {
        lua_pushinteger(state, value.get<lua_Integer>());
    } else if (value.is_number_unsigned()) {
        lua_pushnumber(state, static_cast<double>(value.get<std::uint64_t>()));
    } else if (value.is_number_float()) {
        lua_pushnumber(state, value.get<double>());
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_array()) {
        lua_newtable(state);
        for (std::size_t index = 0; index < value.size(); ++index) {
            push_json(state, value[index], depth + 1, allow_null);
            lua_rawseti(state, -2, static_cast<int>(index + 1));
        }
    } else if (value.is_object()) {
        lua_newtable(state);
        for (const auto& [key, item] : value.items()) {
            push_json(state, item, depth + 1, allow_null);
            lua_setfield(state, -2, key.c_str());
        }
    } else {
        throw std::runtime_error("Playtest action contains unsupported JSON");
    }
}

} // namespace ets::detail
