#pragma once

#include <cmath>
#include <limits>
#include <lua.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace ets {

template<typename T>
struct LuauScalarCodec;

template<typename T>
    requires std::is_arithmetic_v<T>
struct LuauScalarCodec<T> {
    static T read(lua_State* state, int index) {
        if constexpr (std::is_same_v<T, bool>) {
            if (lua_type(state, index) != LUA_TBOOLEAN) {
                throw std::invalid_argument(
                    "Expected boolean at argument " + std::to_string(index)
                );
            }
            return lua_toboolean(state, index) != 0;
        } else {
            if (lua_type(state, index) != LUA_TNUMBER) {
                throw std::invalid_argument(
                    "Expected number at argument " + std::to_string(index)
                );
            }
            const double value = lua_tonumber(state, index);
            if (!std::isfinite(value)) {
                throw std::invalid_argument("Expected finite number");
            }
            if constexpr (std::is_integral_v<T>) {
                constexpr double safe = 9007199254740991.0;
                if (std::trunc(value) != value || value < -safe ||
                    value > safe ||
                    static_cast<long double>(value) <
                        static_cast<long double>(
                            std::numeric_limits<T>::lowest()
                        ) ||
                    static_cast<long double>(value) >
                        static_cast<long double>(
                            std::numeric_limits<T>::max()
                        )) {
                    throw std::out_of_range(
                        "Integer outside exact representable range"
                    );
                }
            } else if (
                value < -std::numeric_limits<T>::max() ||
                value > std::numeric_limits<T>::max()
            ) {
                throw std::out_of_range("Number outside representable range");
            }
            return static_cast<T>(value);
        }
    }
};

template<>
struct LuauScalarCodec<std::string_view> {
    static std::string_view read(lua_State* state, int index) {
        if (lua_type(state, index) != LUA_TSTRING) {
            throw std::invalid_argument(
                "Expected string at argument " + std::to_string(index)
            );
        }
        std::size_t size = 0;
        const char* value = lua_tolstring(state, index, &size);
        return {value, size};
    }
};
template<>
struct LuauScalarCodec<std::string> {
    static std::string read(lua_State* state, int index) {
        return std::string(
            LuauScalarCodec<std::string_view>::read(state, index)
        );
    }
};

} // namespace ets
