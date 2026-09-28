#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace ets::luau::json {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_bytes = 8ULL * 1024 * 1024;
constexpr std::size_t max_nodes = 100'000;
constexpr int max_depth = 64;
constexpr const char* arrays_key = "__ets_json_arrays";
constexpr const char* null_key = "__ets_json_null_value";

std::string string_at(lua_State* state, int index) {
    std::size_t size = 0;
    const auto* data = lua_tolstring(state, index, &size);
    return {data, size};
}
bool is_null(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_getfield(state, LUA_REGISTRYINDEX, null_key);
    const bool result = lua_rawequal(state, index, -1) != 0;
    lua_pop(state, 1);
    return result;
}
bool is_array(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_getfield(state, LUA_REGISTRYINDEX, arrays_key);
    lua_pushvalue(state, index);
    lua_rawget(state, -2);
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 2);
    return result;
}
void mark_array(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_getfield(state, LUA_REGISTRYINDEX, arrays_key);
    lua_pushvalue(state, index);
    lua_pushboolean(state, true);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}
std::size_t array_size(lua_State* state, int index) {
    index = lua_absindex(state, index);
    std::size_t count = 0, maximum = 0;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (lua_type(state, -2) != LUA_TNUMBER) {
            throw std::invalid_argument("Expected a dense array");
        }
        const double key = lua_tonumber(state, -2);
        if (!std::isfinite(key) || key < 1 || key > max_nodes ||
            std::floor(key) != key) {
            throw std::invalid_argument("Invalid array index");
        }
        maximum = std::max(maximum, static_cast<std::size_t>(key));
        ++count;
        lua_pop(state, 1);
    }
    if (count != maximum) {
        throw std::invalid_argument("Expected a dense array");
    }
    return count;
}
struct Encoder {
    lua_State* state;
    std::size_t nodes = 0;
    std::size_t bytes = 0;
    std::unordered_set<const void*> active;
    void account(std::size_t size) {
        bytes += size;
        if (bytes > max_bytes) {
            throw std::invalid_argument("JSON exceeds 8 MiB");
        }
    }
    Json read(int index, const std::string& path, int depth) {
        if (depth > max_depth || ++nodes > max_nodes) {
            throw std::invalid_argument(
                path + ": JSON complexity limit exceeded"
            );
        }
        if (!lua_checkstack(state, 8)) {
            throw std::invalid_argument("JSON stack limit exceeded");
        }
        index = lua_absindex(state, index);
        account(1);
        if (is_null(state, index)) {
            return nullptr;
        }
        switch (lua_type(state, index)) {
            case LUA_TBOOLEAN:
                return lua_toboolean(state, index) != 0;
            case LUA_TNUMBER: {
                const double value = lua_tonumber(state, index);
                if (!std::isfinite(value)) {
                    throw std::invalid_argument(path + ": non-finite number");
                }
                return value;
            }
            case LUA_TSTRING: {
                auto value = string_at(state, index);
                account(value.size());
                return value;
            }
            case LUA_TTABLE:
                break;
            default:
                throw std::invalid_argument(path + ": unsupported JSON value");
        }
        if (lua_getmetatable(state, index)) {
            lua_pop(state, 1);
            throw std::invalid_argument(
                path + ": tables with metatables are not JSON values"
            );
        }
        const auto* pointer = lua_topointer(state, index);
        if (!active.insert(pointer).second) {
            throw std::invalid_argument(path + ": cyclic table");
        }
        bool numeric = false, named = false;
        std::size_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, index)) {
            if (++count > max_nodes) {
                throw std::invalid_argument(path + ": too many entries");
            }
            if (lua_type(state, -2) == LUA_TSTRING) {
                named = true;
            } else if (lua_type(state, -2) == LUA_TNUMBER) {
                numeric = true;
            } else {
                throw std::invalid_argument(path + ": invalid object key");
            }
            lua_pop(state, 1);
        }
        const bool array = numeric || is_array(state, index);
        if (array && named) {
            throw std::invalid_argument(path + ": mixed table keys");
        }
        Json result = array ? Json::array() : Json::object();
        if (array) {
            try {
                count = array_size(state, index);
            } catch (const std::exception& error) {
                throw std::invalid_argument(path + ": " + error.what());
            }
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(state, index, static_cast<int>(i));
                result.push_back(
                    read(-1, path + "[" + std::to_string(i) + "]", depth + 1)
                );
                lua_pop(state, 1);
            }
        } else {
            lua_pushnil(state);
            while (lua_next(state, index)) {
                const auto key = string_at(state, -2);
                account(key.size());
                result[key] = read(
                    -1,
                    path + "[" +
                        Json(key.size() > 80 ? "<long key>" : key).dump() + "]",
                    depth + 1
                );
                lua_pop(state, 1);
            }
        }
        active.erase(pointer);
        return result;
    }
};
void push(lua_State* state, const Json& value) {
    if (!lua_checkstack(state, 8)) {
        throw std::invalid_argument("JSON stack limit exceeded");
    }
    if (value.is_null()) {
        lua_getfield(state, LUA_REGISTRYINDEX, null_key);
    } else if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>());
    } else if (value.is_number()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            throw std::invalid_argument("JSON number out of range");
        }
        lua_pushnumber(state, number);
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(state, text.data(), text.size());
    } else {
        lua_newtable(state);
        if (value.is_array()) {
            mark_array(state, -1);
            int index = 1;
            for (const auto& item : value) {
                push(state, item);
                lua_rawseti(state, -2, index++);
            }
        } else {
            for (auto it = value.begin(); it != value.end(); ++it) {
                lua_pushlstring(state, it.key().data(), it.key().size());
                push(state, it.value());
                lua_rawset(state, -3);
            }
        }
    }
}
int encode(lua_State* state) {
    try {
        const auto value = Encoder {state, 0, 0, {}}.read(1, "$", 0).dump();
        if (value.size() > max_bytes) {
            throw std::invalid_argument("JSON exceeds 8 MiB");
        }
        lua_pushlstring(state, value.data(), value.size());
        return 1;
    } catch (const std::exception& error) {
        luaL_error(state, "%s", error.what());
    }
}
int decode(lua_State* state) {
    luaL_checktype(state, 1, LUA_TSTRING);
    try {
        const auto text = string_at(state, 1);
        if (text.size() > max_bytes) {
            throw std::invalid_argument("JSON exceeds 8 MiB");
        }
        std::size_t nodes = 0;
        auto value =
            Json::parse(text, [&](int depth, Json::parse_event_t event, Json&) {
                if (depth > max_depth ||
                    ((event == Json::parse_event_t::value ||
                      event == Json::parse_event_t::object_start ||
                      event == Json::parse_event_t::array_start) &&
                     ++nodes > max_nodes)) {
                    throw std::invalid_argument(
                        "JSON complexity limit exceeded"
                    );
                }
                return true;
            });
        push(state, value);
        return 1;
    } catch (const std::exception& error) {
        luaL_error(state, "%s", error.what());
    }
}
int array(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    try {
        if (lua_getmetatable(state, 1)) {
            throw std::invalid_argument("Expected a plain dense array");
        }
        const auto count = array_size(state, 1);
        lua_createtable(state, static_cast<int>(count), 0);
        for (std::size_t i = 1; i <= count; ++i) {
            lua_rawgeti(state, 1, static_cast<int>(i));
            lua_rawseti(state, -2, static_cast<int>(i));
        }
        mark_array(state, -1);
        return 1;
    } catch (const std::exception& error) {
        luaL_error(state, "%s", error.what());
    }
}
} // namespace
void Library::open(lua_State* state) {
    lua_newtable(state);
    lua_getfield(state, LUA_REGISTRYINDEX, null_key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newuserdata(state, 1);
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, null_key);
        lua_newtable(state);
        lua_newtable(state);
        lua_pushstring(state, "k");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);
        lua_setfield(state, LUA_REGISTRYINDEX, arrays_key);
    }
    lua_setfield(state, -2, "null");
    for (const auto& [name, function] :
         {std::pair {"encode", encode},
          std::pair {"decode", decode},
          std::pair {"array", array}}) {
        lua_pushcfunction(state, function, name);
        lua_setfield(state, -2, name);
    }
}
} // namespace ets::luau::json
