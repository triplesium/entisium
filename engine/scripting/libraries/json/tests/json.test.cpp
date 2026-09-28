#include "scripting/module_loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE(
    "JSON preserves null, array identity and validates values",
    "[luau][json]"
) {
    ets::LuauRuntime runtime;
    ets::LuauModuleLoader loader(
        runtime,
        [](std::string_view, std::string_view name)
            -> ets::Result<ets::LuauScriptSource, ets::LuauScriptError> {
            return ets::LuauScriptSource {
                .name = std::string(name),
                .content = R"(
            local json = require("@json")
            assert(json.encode({}) == "{}")
            assert(json.encode(json.array({})) == "[]")
            assert(json.decode("null") == json.null)
            assert(json.encode(json.null) == "null")
            local value = json.decode([=[{"a":[null,{},[]],"b":false,"s":"a\u0000b"}]=])
            assert(value.a[1] == json.null and #value.a == 3)
            assert(value.b == false and value.s == "a\0b")
            assert(getmetatable(value.a) == nil)
            assert(json.encode(value.a) == "[null,{},[]]")
            table.remove(value.a, 3); table.remove(value.a, 2); table.remove(value.a, 1)
            assert(json.encode(value.a) == "[]")
            local original = {1,2}
            local copy = json.array(original)
            assert(copy ~= original and copy[1] == 1)
            local shared = {ok=true}
            assert(json.encode({shared, shared}) == '[{"ok":true},{"ok":true}]')
            local function bad(value)
                assert(not pcall(json.encode, value))
                assert(json.encode(true) == "true")
            end
            bad(string.char(255))
            assert(not pcall(json.decode, '"'..string.char(255)..'"'))
            bad(string.rep("\0", 2*1024*1024))
            bad(table.create(100001, true))
            assert(not pcall(json.decode, "["..string.rep("0,",100000).."0]"))
            local nulKey = {["a\0b"] = "value"}
            assert(json.decode(json.encode(nulKey))["a\0b"] == "value")
            bad(nil); bad(0/0); bad(math.huge); bad(function() end)
            bad({[2] = 1}); bad({[0] = 1}); bad({[1] = 1, x = 2})
            bad(setmetatable({}, {})); bad({[true] = 1})
            local cycle = {}; cycle.self = cycle; bad(cycle)
            local ok, message = pcall(json.encode, {nested={invalid=function() end}})
            assert(not ok and string.find(message, "nested", 1, true))
            local root = {}; local next = root
            for i=1,70 do next.child={}; next=next.child end
            bad(root)
            assert(not pcall(json.decode, string.rep("[",70).."0"..string.rep("]",70)))
            assert(not pcall(json.decode, "{invalid}"))
            assert(not pcall(json.decode, "null false"))
            assert(not pcall(json.decode, string.rep(" ", 8*1024*1024+1)))
            assert(not pcall(json.array, {[2]=1}))
            assert(not pcall(json.array, {named=1}))
            return {}
        )",
                .runtime_types = false
            };
        }
    );
    const auto result = loader.load({}, "json-test");
    INFO((result ? "loaded" : result.error().message));
    REQUIRE(result);
}
