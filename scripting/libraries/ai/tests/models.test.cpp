#include "scripting/execution_pool.hpp"
#include "scripting/module_loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <chrono>
#include <httplib.h>
#include <string>
#include <thread>
namespace {
auto configuration(const std::string& model) {
    return ets::LuauHostConfig::parse(
        R"({"version":1,"connections":{"test":{"base_url":"http://127.0.0.1:9/v1","headers":[{"name":"Authorization","value":"Bearer private-test-key"}]}},"generation":{"planner":{"id":")" +
        model +
        R"(","protocol":"chat-completions","connection":"test","timeout":1}},"decisions":{}})"
    );
}
auto load(ets::LuauRuntime& runtime, const std::string& source) {
    ets::LuauModuleLoader loader(
        runtime,
        [&](std::string_view, std::string_view name)
            -> ets::Result<ets::LuauScriptSource, ets::LuauScriptError> {
            return ets::LuauScriptSource {
                .name = std::string(name),
                .content = source,
                .runtime_types = false
            };
        }
    );
    return loader.load({}, "test");
}
} // namespace
TEST_CASE(
    "Model imports keep native configuration interfaces private",
    "[luau][ai][config]"
) {
    ets::LuauRuntime first(configuration("first")),
        second(configuration("second"));
    for (auto* runtime : {&first, &second}) {
        const auto result = load(
            *runtime,
            std::string("local expected = \"") +
                (runtime == &first ? "first" : "second") + R"("
            local ai = require("@ai")
            local http = require("@http")
            local json = require("@json")
            assert(ai.model({model="planner"}))
            assert(__ets_model_binding == nil)
            assert(not pcall(require, "@internal/ai"))
            assert(not pcall(require, "@internal/http"))
            return {}
        )"
        );
        INFO((result ? "loaded" : result.error().message));
        REQUIRE(result);
    }
}
TEST_CASE(
    "Model imports do not require configuration but use reports missing "
    "aliases",
    "[luau][ai][config]"
) {
    ets::LuauRuntime runtime;
    auto result = load(runtime, R"(
        local ai = require("@ai")
        local ok, message = pcall(ai.model, {model="missing"})
        assert(not ok and string.find(message, "configuration is missing", 1, true))
        return {}
    )");
    INFO((result ? "loaded" : result.error().message));
    REQUIRE(result);
    ets::LuauExecutionPool pool(2, configuration("pooled"));
    for (std::size_t index = 0; index < 2; ++index) {
        auto lane = pool.runtime(index);
        REQUIRE(lane);
        auto loaded = load(*lane, R"(
            local ai = require("@ai")
            assert(ai.model({model="planner"}))
            local ok, message = pcall(ai.model, {model="missing"})
            assert(not ok and string.find(message, "Unknown generation model alias", 1, true))
            return {}
        )");
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
    }
}
TEST_CASE(
    "Invalid startup configuration never echoes credentials",
    "[luau][ai][config]"
) {
    REQUIRE_THROWS_WITH(
        ets::LuauHostConfig::parse(R"({"version":99,"apiKey":"private-key"})"),
        "Invalid Luau host configuration"
    );
}

TEST_CASE(
    "Game VM performs LLM requests without a Node host",
    "[luau][ai][game]"
) {
    httplib::Server server;
    server.Post(
        "/v1/chat/completions",
        [](const httplib::Request& request, httplib::Response& response) {
            if (request.get_header_value("Authorization") !=
                "Bearer private-test-key") {
                response.status = 401;
                return;
            }
            response.set_content(
                R"({"model":"game-model","choices":[{"message":{"role":"assistant","content":"game answer"},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":1}})",
                "application/json"
            );
        }
    );
    const auto port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::jthread worker([&] {
        server.listen_after_bind();
    });
    struct Stop {
        httplib::Server& server;
        ~Stop() { server.stop(); }
    } stop {server};
    server.wait_until_ready();
    auto config =
        std::make_shared<ets::LuauHostConfig>(*configuration("game-request"));
    config->connections.at("test").base_url =
        "http://127.0.0.1:" + std::to_string(port) + "/v1";
    ets::LuauRuntime runtime(config);
    const std::string source = R"(
        local ai = require("@ai")
        local task = require("@task")
        local session = task.start(function()
            local model = ai.model {model="planner"}
            local result = model:generate {prompt="game"}:await()
            assert(result.data == "game answer" and result.model == "game-model")
        end)
        local function tick() session.step() end
        local function verify() assert(session.step()); session.close() end
        export local AiPlugin = Plugin.new {
            build = function(app: App)
                app:add_system(Update, tick)
                app:add_system(Update, verify)
            end,
        }
    )";
    auto artifact = ets::compile_luau_script_module(
        {.name = "game-ai.luau", .content = source}
    );
    INFO((artifact ? "compiled" : artifact.error().message));
    REQUIRE(artifact);
    auto module = runtime.load_module(*artifact);
    INFO((module ? "loaded" : module.error().message));
    REQUIRE(module);
    for (int i = 0; i < 300; ++i) {
        const auto tick = runtime.call_module_function(*module, "tick");
        INFO((tick ? "tick" : tick.error().message));
        REQUIRE(tick);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto verified = runtime.call_module_function(*module, "verify");
    INFO((verified ? "verified" : verified.error().message));
    REQUIRE(verified);
}
