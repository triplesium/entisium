#include "../http.hpp"

#include "scripting/module_loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <httplib.h>
#include <string>
#include <thread>
TEST_CASE(
    "HTTP runs in the game VM without Node or host-call mode",
    "[luau][http]"
) {
    httplib::Server server;
    server.Post(
        "/echo",
        [](const httplib::Request& request, httplib::Response& response) {
            response.status = 409;
            response.set_content(request.body, "text/plain");
        }
    );
    const auto port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::jthread server_thread([&] {
        server.listen_after_bind();
    });
    struct Stop {
        httplib::Server& server;
        ~Stop() { server.stop(); }
    } stop {server};
    server.wait_until_ready();
    ets::LuauRuntime runtime;
    const std::string source =
        "local url = \"http://127.0.0.1:" + std::to_string(port) + "/echo\"\n" +
        R"(
        local task = require("@task")
        local http = require("@http")
        local json = require("@json")
        local session = task.start(function()
            local response = http.request({url = url, method = "POST", body = "a\0b"}):await()
            assert(response.status == 409 and response.body == "a\0b")
            assert(type(response) == "userdata" and type(response.headers) == "userdata")
            assert(#response.headers > 0 and type(response.headers[1].name) == "string")
            for _, header in response.headers do assert(type(header.value) == "string") end
            local bad = pcall(function() http.request({url = "file:///invalid"}) end)
            assert(not bad)
            local jsonResponse = http.request({url = url, method = "POST",
                headers = {{name = "Content-Type", value = "application/json"}},
                body = json.encode({items = json.array({}), missing = json.null})
            }):await()
            local decoded = json.decode(jsonResponse.body)
            assert(decoded.missing == json.null and json.encode(decoded.items) == "[]")
            local pollCount = 0
            local value = task.poll(function()
                pollCount += 1
                return pollCount > 5000, 42
            end):await()
            assert(value == 42)
        end)
        local function tick() session.step() end
        local function verify() assert(session.step()); session.close() end
        local function cancel()
            local cancelled = false
            local pending = task.start(function()
                task.on_cleanup(function() cancelled = true end)
                http.request({url = url})
                task.sleep(60):await()
            end)
            assert(not pending.step())
            pending.close()
            assert(cancelled and pending.step())
            local nextSession = task.start(function() end)
            assert(nextSession.step())
        end
        export local HttpPlugin = Plugin.new {
            build = function(app: App)
                app:add_system(Update, tick)
                app:add_system(Update, verify)
                app:add_system(Update, cancel)
            end,
        }
    )";
    const auto game_artifact = ets::compile_luau_script_module(
        {.name = "http-game.luau", .content = source}
    );
    INFO((game_artifact ? "game compiled" : game_artifact.error().message));
    REQUIRE(game_artifact);
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
    auto module = loader.load({}, "http-test");
    INFO((module ? "loaded" : module.error().message));
    REQUIRE(module);
    for (int i = 0; i < 5100; ++i) {
        const auto result = runtime.call_module_function(*module, "tick");
        INFO((result ? "tick" : result.error().message));
        REQUIRE(result);
        if (i < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    auto verified = runtime.call_module_function(*module, "verify");
    INFO((verified ? "verified" : verified.error().message));
    REQUIRE(verified);
    auto cancelled = runtime.call_module_function(*module, "cancel");
    INFO((cancelled ? "cancelled" : cancelled.error().message));
    REQUIRE(cancelled);
}

TEST_CASE(
    "Typed HTTP handles preserve identity and instance ownership",
    "[luau][http]"
) {
    auto config = std::make_shared<ets::LuauHostConfig>();
    config->connections["test"].base_url = "http://127.0.0.1:9";
    ets::LuauLibraryServices services {.config = config};
    ets::luau::http::Library first(services), second(services);
    auto handle = first.connection("test");
    auto copied = handle;
    CHECK(handle == copied);
    CHECK(handle == first.connection("test"));
    CHECK_FALSE(handle == second.connection("test"));
    CHECK_THROWS(second.close(handle));
    CHECK_THROWS(second.submit({.path = "/", .connection = copied}));
    first.close(copied);
    CHECK_THROWS(first.connection("test"));
    CHECK_THROWS(first.submit({.path = "/", .connection = handle}));
    CHECK_NOTHROW(second.connection("test"));
}
