#include "scripting/module_loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE(
    "Context core loads without filesystem or host services",
    "[luau][context]"
) {
    ets::LuauRuntime runtime;
    ets::LuauModuleLoader loader(
        runtime,
        [](std::string_view, std::string_view name)
            -> ets::Result<ets::LuauScriptSource, ets::LuauScriptError> {
            if (name != "context-test") {
                return ets::failure(
                    ets::LuauScriptError {
                        "Unexpected source resolution: " + std::string(name)
                    }
                );
            }
            return ets::LuauScriptSource {
                .name = std::string(name),
                .content = R"(
                local core = require("@context/core")
                local json = require("@json")
                assert(core == require("@context/core"))
                local context = core.bind({
                    sample = function(requests)
                        assert(#requests == 0)
                        return {frame = 7, simulation_time = 0.5, batch = {}}
                    end,
                    encode = json.encode,
                })
                local view = context.view {fields = {
                    score = context.value(21):map(function(value) return value * 2 end),
                }}
                local sample = view:sample()
                assert(sample.data.score == 42)
                assert(sample.frame == 7)
                assert(json.decode(sample:encode()).data.score == 42)
                return {}
            )",
                .runtime_types = false,
            };
        }
    );
    const auto result = loader.load({}, "context-test");
    INFO((result ? "loaded" : result.error().message));
    REQUIRE(result);
}
