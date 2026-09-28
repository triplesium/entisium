#include "scripting/module_loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <unordered_map>

TEST_CASE(
    "Tool modules cache exports and return resumable entry functions",
    "[scripting][modules][host]"
) {
    using namespace ets;
    std::unordered_map<std::string, std::string> sources {
        {"./helper", "local state = {}\nreturn state"},
        {"./main", R"(
            local first = require("./helper")
            local second = require("./helper")
            return function()
                assert(first == second)
                local reply = coroutine.yield({kind = "request", method = "example", payload = {}})
                assert(reply.ok and reply.value.answer == 42 and reply.value.absent == nil)
            end
        )"},
        {"./cycle", "local cycle = require(\"./cycle\")\nreturn {}"},
    };
    LuauRuntime runtime;
    runtime.enable_host_calls();
    LuauModuleLoader loader(
        runtime,
        [&](std::string_view, std::string_view specifier)
            -> Result<LuauScriptSource, LuauScriptError> {
            auto found = sources.find(std::string(specifier));
            if (found == sources.end())
                return failure(LuauScriptError {"Missing module"});
            return LuauScriptSource {
                .name = found->first,
                .content = found->second
            };
        }
    );
    CHECK_FALSE(loader.load({}, "./cycle"));
    auto module = loader.load({}, "./main");
    REQUIRE(module);
    REQUIRE(runtime.start_task(*module));
    auto request = runtime.resume_task();
    REQUIRE(request);
    CHECK(request->find("example") != std::string::npos);
    auto done = runtime.resume_task(
        R"({"ok":true,"value":{"answer":42,"absent":null}})"
    );
    REQUIRE(done);
    CHECK(*done == R"({"kind":"completed"})");
}
