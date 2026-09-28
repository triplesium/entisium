#include "scripting/library.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <lua.h>
#include <lualib.h>

TEST_CASE(
    "Host catalog contains only selected library targets",
    "[luau][catalog]"
) {
    CHECK(ets::default_luau_libraries().size() == 2);
    CHECK(ets::is_luau_library("@fixture"));
    CHECK(ets::is_luau_library("@multifile"));
    CHECK_FALSE(ets::is_luau_library("@http"));
    // JSON is selected only by this host's SDK target.
    CHECK_FALSE(ets::is_luau_library("@json"));
    CHECK(
        std::filesystem::is_regular_file(
            std::filesystem::path(ETS_TEST_SDK_DIRECTORY) / "json/init.luau"
        )
    );
    for (int i = 0; i < 2; ++i) {
        auto* state = luaL_newstate();
        luaL_openlibs(state);
        luaL_sandbox(state);
        ets::LuauLibraries libraries(state, {});
        CHECK_NOTHROW(libraries.require(state, "@multifile"));
        lua_getfield(state, -1, "value");
        CHECK(lua_tonumber(state, -1) == 42);
        CHECK_THROWS(libraries.require(state, "@multifile/value"));
        libraries.close();
        lua_close(state);
    }
}

TEST_CASE(
    "Private source cycles fail without poisoning retries",
    "[luau][catalog]"
) {
    const std::array<ets::LuauLibraryDefinition, 1> definitions {
        {{.name = "@cycle",
          .source = "return require('./a')",
          .modules = {
              {"a.luau", "return require('./b')"},
              {"b.luau", "return require('./a')"}
          }}}
    };
    auto* state = luaL_newstate();
    luaL_openlibs(state);
    luaL_sandbox(state);
    ets::LuauLibraries libraries(state, {}, definitions);
    for (int i = 0; i < 2; ++i) {
        const auto top = lua_gettop(state);
        CHECK_THROWS(libraries.require(state, "@cycle"));
        CHECK(lua_gettop(state) == top);
    }
    libraries.close();
    lua_close(state);
}
