#include "scripting/type_descriptor.hpp"

#include "refl/cls.hpp"
#include "refl/registry.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ets {
namespace {
nlohmann::json describe(TypeId id, unsigned depth) {
    if (depth > 16) {
        throw std::runtime_error("Type description exceeds nesting limit");
    }
    auto type = Registry::instance().try_get_type(id);
    if (!type) {
        throw std::runtime_error(type.error().message);
    }
    nlohmann::json result {{"name", type->name()}};
    if (auto cls = Registry::instance().try_get_cls(id)) {
        auto fields = nlohmann::json::object();
        for (const auto* field : cls->get_properties()) {
            fields[field->name()] = describe(field->type_id(), depth + 1);
        }
        result["fields"] = std::move(fields);
    }
    return result;
}
} // namespace
std::string luau_type_descriptor(TypeId type) {
    return describe(type, 0).dump();
}
} // namespace ets
