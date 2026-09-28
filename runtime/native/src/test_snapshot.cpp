#include "runtime_host/test_snapshot.hpp"

#include "ecs/world.hpp"
#include "refl/registry.hpp"
#include "scripting/type_descriptor.hpp"
#include "serialization/json_archive.hpp"
#include "serialization/serializer.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ets::runtime_host {
namespace {
using Json = nlohmann::json;
TypeId resolve_type(const Json& descriptor) {
    auto type = Registry::instance().try_get_type_exact(
        descriptor.at("name").get<std::string>()
    );
    if (!type) {
        throw std::runtime_error(type.error().message);
    }
    if (Json::parse(luau_type_descriptor(type->id())) != descriptor) {
        throw std::runtime_error(
            "Type structure differs between test and game: " + type->name()
        );
    }
    return type->id();
}
Json snapshot(Ref ref) {
    auto node = serialization::serialize(ref, {.include_type_tag = false});
    if (!node) {
        throw std::runtime_error(
            node.error().path + ": " + node.error().message
        );
    }
    auto text = serialization::write_json(*node, -1);
    if (!text) {
        throw std::runtime_error(text.error().message);
    }
    return Json::parse(*text);
}
} // namespace
std::string
inspect_test_snapshot(const World& world, std::string_view payload) {
    const auto request = Json::parse(payload);
    if (!request.is_object()) {
        throw std::runtime_error("Snapshot request must be an object");
    }
    if (request.contains("batch")) {
        const auto& batch = request.at("batch");
        if (request.size() != 1 || !batch.is_array() || batch.empty() ||
            batch.size() > 32) {
            throw std::runtime_error(
                "Snapshot batch must contain 1-32 requests"
            );
        }
        auto results = Json::array();
        for (const auto& item : batch) {
            if (item.contains("batch")) {
                throw std::runtime_error(
                    "Nested snapshot batches are unsupported"
                );
            }
            results.push_back(
                Json::parse(inspect_test_snapshot(world, item.dump()))
            );
        }
        auto text = Json {{"batch", results}}.dump();
        if (text.size() > 1024ULL * 1024) {
            throw std::runtime_error("Snapshot batch exceeds 1 MiB");
        }
        return text;
    }
    for (const auto& [key, value] : request.items()) {
        if (key == "optional") {
            if (!value.is_boolean() || !request.contains("resources")) {
                throw std::runtime_error(
                    "optional requires a resource request and boolean value"
                );
            }
            continue;
        }
        if (key != "resources" && key != "components") {
            throw std::runtime_error("Unknown snapshot field: " + key);
        }
        if (!value.is_array() || value.empty() || value.size() > 32) {
            throw std::runtime_error(
                "Snapshot type lists must contain 1-32 types"
            );
        }
    }
    Json result {{"resources", Json::array()}, {"entities", Json::array()}};
    if (request.contains("resources")) {
        for (const auto& descriptor : request.at("resources")) {
            const auto id = resolve_type(descriptor);
            if (!world.has_resource(id)) {
                if (request.value("optional", false)) {
                    result["resources"].push_back(nullptr);
                    continue;
                }
                throw std::runtime_error(
                    "Resource is not installed: " +
                    descriptor.at("name").get<std::string>()
                );
            }
            result["resources"].push_back(snapshot(world.resource(id)));
        }
    }
    if (request.contains("components")) {
        std::vector<TypeId> types;
        for (const auto& descriptor : request.at("components")) {
            types.push_back(resolve_type(descriptor));
        }
        for (const auto& [id, archetype] : world.archetypes()) {
            (void)id;
            if (!std::ranges::all_of(types, [&](TypeId type) {
                    return std::ranges::find(archetype.components(), type) !=
                           archetype.components().end();
                })) {
                continue;
            }
            for (std::size_t row = 0; row < archetype.entities().size();
                 ++row) {
                if (result["entities"].size() >= 4096) {
                    throw std::runtime_error("Query exceeds 4096 entities");
                }
                auto values = Json::array();
                for (auto type : types) {
                    values.push_back(
                        snapshot(archetype.get_component(type, row))
                    );
                }
                result["entities"].push_back(
                    Json {
                        {"entity", archetype.entities()[row].value},
                        {"components", std::move(values)}
                    }
                );
            }
        }
        auto& rows = result["entities"].get_ref<Json::array_t&>();
        std::ranges::sort(rows, {}, [](const Json& row) {
            return row.at("entity").get<std::uint32_t>();
        });
    }
    auto text = result.dump();
    if (text.size() > 1024ULL * 1024) {
        throw std::runtime_error("Snapshot exceeds 1 MiB");
    }
    return text;
}
} // namespace ets::runtime_host
