#include "runtime_host/test_snapshot.hpp"

#include "ecs/world.hpp"
#include "refl/cls.hpp"
#include "refl/registry.hpp"
#include "scripting/type_descriptor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
struct TestPosition {
    float x;
};
} // namespace
TEST_CASE(
    "External tests read typed snapshots without changing the world",
    "[runtime-host][playtest]"
) {
    using namespace ets;
    using Json = nlohmann::json;
    Registry::instance()
        .register_cls<TestPosition>({}, "ExternalTestPosition")
        .add_property("x", &TestPosition::x);
    World world;
    world.add_resource(TestPosition {2.0f});
    const auto descriptor =
        Json::parse(luau_type_descriptor(type_id<TestPosition>()));
    const auto request = Json {{"resources", Json::array({descriptor})}};
    const auto before = world.resource_ticks(type_id<TestPosition>()).changed;
    const auto snapshot =
        Json::parse(runtime_host::inspect_test_snapshot(world, request.dump()));
    CHECK(snapshot.at("resources")[0].at("x") == 2.0f);
    CHECK(world.resource_ticks(type_id<TestPosition>()).changed == before);
    auto wrong = request;
    wrong["resources"][0]["fields"]["x"]["name"] = "wrong";
    CHECK_THROWS(runtime_host::inspect_test_snapshot(world, wrong.dump()));
    auto batch = Json {{"batch", Json::array({request, request})}};
    const auto combined =
        Json::parse(runtime_host::inspect_test_snapshot(world, batch.dump()));
    CHECK(combined["batch"][0] == combined["batch"][1]);
    CHECK(world.resource_ticks(type_id<TestPosition>()).changed == before);
    World empty;
    auto optional = request;
    optional["optional"] = true;
    CHECK(
        Json::parse(
            runtime_host::inspect_test_snapshot(empty, optional.dump())
        )["resources"][0]
            .is_null()
    );
    CHECK_THROWS(runtime_host::inspect_test_snapshot(empty, request.dump()));
    wrong["optional"] = true;
    CHECK_THROWS(runtime_host::inspect_test_snapshot(empty, wrong.dump()));
    CHECK_THROWS(
        runtime_host::inspect_test_snapshot(
            world,
            Json {{"batch", Json::array({batch})}}.dump()
        )
    );
}
