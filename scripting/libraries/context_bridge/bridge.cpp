#include "bridge.hpp"

#include "scripting/type_descriptor.hpp"
#include "serialization/json_archive.hpp"
#include "serialization/serializer.hpp"

#include <nlohmann/json.hpp> // IWYU pragma: keep
#include <stdexcept>
namespace ets::luau::context_bridge {
Library::Library(LuauLibraryServices& services) {
    if (!services.host_calls) {
        throw std::runtime_error("Context bridge requires host calls");
    }
}
nlohmann::json Library::type_descriptor(TypeId type) {
    return nlohmann::json::parse(luau_type_descriptor(type));
}
const Val Library::decode_snapshot(TypeId type, std::string text) {
    auto node = serialization::read_json(text);
    if (!node) {
        throw std::runtime_error(node.error().message);
    }
    auto value = serialization::deserialize(type, *node);
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
    return std::move(*value);
}
std::string Library::encode(nlohmann::json value) {
    auto text = value.dump();
    if (text.size() > 1024ULL * 1024) {
        throw std::runtime_error("Encoded context exceeds 1 MiB");
    }
    return text;
}
const Val Library::copy_value(Val value) {
    return value;
}
} // namespace ets::luau::context_bridge
