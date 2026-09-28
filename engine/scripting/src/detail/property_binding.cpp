#include "binding_internal.hpp"
#include "refl/callable.hpp"
#include "refl/cls.hpp"
#include "refl/registry.hpp"
#include "scripting/compiler.hpp"
#include "scripting/detail/asset_server_binding.hpp"
#include "scripting/detail/binding.hpp"
#include "scripting/detail/commands_binding.hpp"
#include "scripting/detail/reflection_bridge.hpp"
#include "scripting/detail/state.hpp"
#include "scripting/detail/world_binding.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <lua.h>
#include <lualib.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ets::detail {
namespace {

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view> {}(value);
    }
};

enum class LuauPrimitiveKind {
    None,
    Entity,
    Boolean,
    Float,
    Double,
    SignedChar,
    UnsignedChar,
    Short,
    UnsignedShort,
    Int,
    UnsignedInt,
    Long,
    UnsignedLong,
    LongLong,
    UnsignedLongLong,
    String,
};

struct LuauPropertyBinding {
    Property* property {nullptr};
    LuauPrimitiveKind primitive_kind {LuauPrimitiveKind::None};
};

using LuauPropertyMap = std::unordered_map<
    std::string,
    LuauPropertyBinding,
    TransparentStringHash,
    std::equal_to<>>;

struct LuauClassBinding {
    Cls* cls {nullptr};
    std::uint64_t property_revision {0};
    LuauPropertyMap properties;
};

struct LuauDirectPropertyPath {
    std::string atom_name;
    TypeId root_type;
    TypeId leaf_type;
    LuauPrimitiveKind primitive_kind {LuauPrimitiveKind::None};
    std::vector<std::string> property_names;
    std::vector<Property*> properties;
    std::vector<Cls*> property_owners;
    std::vector<std::uint64_t> property_revisions;
    Optional<std::size_t> offset;
};

struct LuauBindingCache {
    std::uint64_t class_epoch {0};
    std::unordered_map<TypeId, LuauClassBinding> classes;
    std::unordered_map<std::string, std::int16_t> direct_atoms;
    std::vector<LuauDirectPropertyPath> direct_paths =
        std::vector<LuauDirectPropertyPath>(1);
};

void destroy_binding_cache(void* userdata) {
    static_cast<LuauBindingCache*>(userdata)->~LuauBindingCache();
}

LuauBindingCache& direct_binding_cache(lua_State* state) {
    auto* cache =
        static_cast<LuauBindingCache*>(lua_callbacks(state)->userdata);
    if (cache == nullptr) {
        luaL_error(state, "Luau direct property cache is unavailable");
    }
    return *cache;
}

std::int16_t
direct_property_atom(lua_State* state, const char* text, std::size_t length) {
    auto* cache =
        static_cast<LuauBindingCache*>(lua_callbacks(state)->userdata);
    if (cache == nullptr) {
        return -1;
    }
    const auto atom = cache->direct_atoms.find(std::string {text, length});
    return atom != cache->direct_atoms.end() ? atom->second : -1;
}

int raise_message(lua_State* state, const std::string& message) {
    luaL_error(state, "%s", message.c_str());
    return 0;
}

LuauBindingCache& binding_cache(lua_State* state) {
    auto* cache = static_cast<LuauBindingCache*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (cache == nullptr) {
        luaL_error(state, "Luau reflection binding cache is unavailable");
    }
    return *cache;
}

LuauPrimitiveKind primitive_kind(TypeId type) {
    if (type == type_id<Entity>()) {
        return LuauPrimitiveKind::Entity;
    }
    if (type == type_id<bool>()) {
        return LuauPrimitiveKind::Boolean;
    }
    if (type == type_id<float>()) {
        return LuauPrimitiveKind::Float;
    }
    if (type == type_id<double>()) {
        return LuauPrimitiveKind::Double;
    }
    if (type == type_id<signed char>()) {
        return LuauPrimitiveKind::SignedChar;
    }
    if (type == type_id<unsigned char>()) {
        return LuauPrimitiveKind::UnsignedChar;
    }
    if (type == type_id<short>()) {
        return LuauPrimitiveKind::Short;
    }
    if (type == type_id<unsigned short>()) {
        return LuauPrimitiveKind::UnsignedShort;
    }
    if (type == type_id<int>()) {
        return LuauPrimitiveKind::Int;
    }
    if (type == type_id<unsigned int>()) {
        return LuauPrimitiveKind::UnsignedInt;
    }
    if (type == type_id<long>()) {
        return LuauPrimitiveKind::Long;
    }
    if (type == type_id<unsigned long>()) {
        return LuauPrimitiveKind::UnsignedLong;
    }
    if (type == type_id<long long>()) {
        return LuauPrimitiveKind::LongLong;
    }
    if (type == type_id<unsigned long long>()) {
        return LuauPrimitiveKind::UnsignedLongLong;
    }
    if (type == type_id<std::string>()) {
        return LuauPrimitiveKind::String;
    }
    return LuauPrimitiveKind::None;
}

Result<LuauPropertyBinding&, std::string>
resolve_property(LuauBindingCache& cache, TypeId type, std::string_view name) {
    auto& registry = Registry::instance();
    if (cache.class_epoch != registry.class_epoch()) {
        cache.classes.clear();
        cache.class_epoch = registry.class_epoch();
    }

    auto cached_class = cache.classes.find(type);
    if (cached_class == cache.classes.end()) {
        auto cls = registry.try_get_cls(type);
        if (!cls) {
            return failure(std::move(cls.error().message));
        }
        cached_class =
            cache.classes
                .emplace(
                    type,
                    LuauClassBinding {
                        .cls = &*cls,
                        .property_revision = cls->property_revision(),
                    }
                )
                .first;
    } else if (
        cached_class->second.property_revision !=
        cached_class->second.cls->property_revision()
    ) {
        cached_class->second.properties.clear();
        cached_class->second.property_revision =
            cached_class->second.cls->property_revision();
    }

    auto cached_property = cached_class->second.properties.find(name);
    if (cached_property != cached_class->second.properties.end()) {
        return cached_property->second;
    }
    auto property =
        cached_class->second.cls->try_get_property(std::string {name});
    if (!property) {
        return failure(std::move(property.error().message));
    }
    auto [cached, inserted] = cached_class->second.properties.emplace(
        std::string {name},
        LuauPropertyBinding {
            .property = &*property,
            .primitive_kind = primitive_kind(property->type_id()),
        }
    );
    static_cast<void>(inserted);
    return cached->second;
}

void push_property_ref(
    lua_State* state,
    Ref ref,
    const LuauObjectView& parent,
    LuauPrimitiveKind kind
) {
    switch (kind) {
        case LuauPrimitiveKind::Entity:
            lua_pushunsigned(state, ref.get_const<Entity>().value);
            return;
        case LuauPrimitiveKind::Boolean:
            lua_pushboolean(state, ref.get_const<bool>());
            return;
        case LuauPrimitiveKind::Float:
            lua_pushnumber(state, static_cast<double>(ref.get_const<float>()));
            return;
        case LuauPrimitiveKind::Double:
            lua_pushnumber(state, ref.get_const<double>());
            return;
        case LuauPrimitiveKind::SignedChar:
            lua_pushinteger(state, ref.get_const<signed char>());
            return;
        case LuauPrimitiveKind::UnsignedChar:
            lua_pushinteger(state, ref.get_const<unsigned char>());
            return;
        case LuauPrimitiveKind::Short:
            lua_pushinteger(state, ref.get_const<short>());
            return;
        case LuauPrimitiveKind::UnsignedShort:
            lua_pushinteger(state, ref.get_const<unsigned short>());
            return;
        case LuauPrimitiveKind::Int:
            lua_pushinteger(state, ref.get_const<int>());
            return;
        case LuauPrimitiveKind::UnsignedInt:
            lua_pushinteger(
                state,
                static_cast<lua_Integer>(ref.get_const<unsigned int>())
            );
            return;
        case LuauPrimitiveKind::Long:
            lua_pushinteger(state, ref.get_const<long>());
            return;
        case LuauPrimitiveKind::UnsignedLong:
            lua_pushinteger(
                state,
                static_cast<lua_Integer>(ref.get_const<unsigned long>())
            );
            return;
        case LuauPrimitiveKind::LongLong:
            lua_pushinteger(
                state,
                static_cast<lua_Integer>(ref.get_const<long long>())
            );
            return;
        case LuauPrimitiveKind::UnsignedLongLong:
            lua_pushinteger(
                state,
                static_cast<lua_Integer>(ref.get_const<unsigned long long>())
            );
            return;
        case LuauPrimitiveKind::String: {
            const auto& value = ref.get_const<std::string>();
            lua_pushlstring(state, value.data(), value.size());
            return;
        }
        case LuauPrimitiveKind::None:
            push_luau_ref(state, ref, parent);
            return;
    }
}

struct PropertyAssignment {
    bool handled {false};
    bool changed {false};
};

Result<bool, InvokeFailure>
set_property_if_changed(Property& property, Ref object, Ref value) {
    auto current = property.get(object);
    if (!current) {
        return failure(std::move(current.error()));
    }
    if (luau_reflected_values_equal(*current, value)) {
        return false;
    }
    auto assigned = property.set(object, value);
    if (!assigned) {
        return failure(std::move(assigned.error()));
    }
    return true;
}

template<typename T>
Result<PropertyAssignment, InvokeFailure>
set_property_value(Property& property, Ref object, T value) {
    auto changed = set_property_if_changed(property, object, Ref(value));
    if (!changed) {
        return failure(std::move(changed.error()));
    }
    return PropertyAssignment {.handled = true, .changed = *changed};
}

Result<PropertyAssignment, InvokeFailure> try_set_primitive_property(
    lua_State* state,
    int index,
    Ref object,
    const LuauPropertyBinding& binding
) {
    const auto incompatible =
        [&]() -> Result<PropertyAssignment, InvokeFailure> {
        return failure(
            InvokeFailure::invalid_call(
                "value is incompatible with reflected type '" +
                type_name(binding.property->type_id()) + "'"
            )
        );
    };
    switch (binding.primitive_kind) {
        case LuauPrimitiveKind::Entity:
            if (!lua_isnumber(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                Entity {
                    static_cast<std::uint32_t>(lua_tounsigned(state, index)),
                }
            );
        case LuauPrimitiveKind::Boolean:
            if (!lua_isboolean(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                lua_toboolean(state, index) != 0
            );
        case LuauPrimitiveKind::Float:
            if (!lua_isnumber(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                static_cast<float>(lua_tonumber(state, index))
            );
        case LuauPrimitiveKind::Double:
            if (!lua_isnumber(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                lua_tonumber(state, index)
            );
        case LuauPrimitiveKind::Int:
            if (!lua_isnumber(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                static_cast<int>(lua_tointeger(state, index))
            );
        case LuauPrimitiveKind::UnsignedInt:
            if (!lua_isnumber(state, index)) {
                return incompatible();
            }
            return set_property_value(
                *binding.property,
                object,
                lua_tounsigned(state, index)
            );
        case LuauPrimitiveKind::String: {
            if (!lua_isstring(state, index)) {
                return incompatible();
            }
            std::size_t size = 0;
            const char* text = lua_tolstring(state, index, &size);
            return set_property_value(
                *binding.property,
                object,
                std::string(text, size)
            );
        }
        case LuauPrimitiveKind::None:
        case LuauPrimitiveKind::SignedChar:
        case LuauPrimitiveKind::UnsignedChar:
        case LuauPrimitiveKind::Short:
        case LuauPrimitiveKind::UnsignedShort:
        case LuauPrimitiveKind::Long:
        case LuauPrimitiveKind::UnsignedLong:
        case LuauPrimitiveKind::LongLong:
        case LuauPrimitiveKind::UnsignedLongLong:
            return PropertyAssignment {};
    }
    return PropertyAssignment {};
}

LuauDirectPropertyPath&
direct_property_path(lua_State* state, int atom, std::uint16_t* cached_slot) {
    auto& cache = direct_binding_cache(state);
    std::size_t slot = *cached_slot;
    if (slot == 0) {
        if (atom <= 0 ||
            static_cast<std::size_t>(atom) >= cache.direct_paths.size()) {
            luaL_error(state, "unknown Luau direct property atom %d", atom);
        }
        slot = static_cast<std::size_t>(atom);
        *cached_slot = static_cast<std::uint16_t>(slot);
    }
    if (slot >= cache.direct_paths.size()) {
        luaL_error(state, "invalid Luau direct property cache slot");
    }
    return cache.direct_paths[slot];
}

Result<Ref, std::string>
direct_property_ref(LuauDirectPropertyPath& path, Ref root) {
    if (!root || root.type_id() != path.root_type) {
        return failure(
            std::string {
                "direct property root type does not match compiled path"
            }
        );
    }
    if (!path.offset) {
        Ref leaf = root;
        for (Property* property : path.properties) {
            auto value = property->get(leaf);
            if (!value) {
                return failure(std::move(value.error().message));
            }
            leaf = *value;
        }
        if (!leaf || leaf.type_id() != path.leaf_type) {
            return failure(
                std::string {
                    "direct property leaf type does not match compiled path"
                }
            );
        }
        auto root_type = Registry::instance().try_get_type(path.root_type);
        auto leaf_type = Registry::instance().try_get_type(path.leaf_type);
        if (!root_type || !leaf_type) {
            return failure(
                std::string {"direct property reflected type is unavailable"}
            );
        }
        const auto root_address =
            reinterpret_cast<std::uintptr_t>(root.const_ptr());
        const auto leaf_address =
            reinterpret_cast<std::uintptr_t>(leaf.const_ptr());
        const std::size_t root_size = root_type->size();
        const std::size_t leaf_size = leaf_type->size();
        if (leaf_address < root_address ||
            leaf_address - root_address > root_size ||
            leaf_size > root_size - (leaf_address - root_address)) {
            return failure(
                std::string {
                    "direct property does not refer to storage inside its root "
                    "object"
                }
            );
        }
        path.offset = static_cast<std::size_t>(leaf_address - root_address);
    }

    if (root.is_const()) {
        const auto* base = static_cast<const std::byte*>(root.const_ptr());
        return Ref(base + *path.offset, path.leaf_type);
    }
    auto* base = static_cast<std::byte*>(root.ptr());
    return Ref(base + *path.offset, path.leaf_type);
}

void direct_property_get(
    lua_State* state,
    const LuauObjectView& object,
    int atom,
    std::uint16_t* cached_slot
) {
    auto& path = direct_property_path(state, atom, cached_slot);
    auto value = direct_property_ref(path, object.ref);
    if (!value) {
        raise_message(state, value.error());
        return;
    }
    push_property_ref(state, *value, object, path.primitive_kind);
}

template<typename T>
bool assign_direct_number(lua_State* state, Ref target) {
    if (!lua_isnumber(state, 3)) {
        luaL_typeerror(state, 3, "number");
    }
    T value;
    if constexpr (std::is_floating_point_v<T>) {
        value = static_cast<T>(lua_tonumber(state, 3));
    } else if constexpr (std::is_unsigned_v<T>) {
        value = static_cast<T>(lua_tounsigned(state, 3));
    } else {
        value = static_cast<T>(lua_tointeger(state, 3));
    }
    auto& current = target.get<T>();
    if (current == value) {
        return false;
    }
    current = value;
    return true;
}

bool assign_direct_primitive(
    lua_State* state,
    Ref target,
    LuauPrimitiveKind kind
) {
    switch (kind) {
        case LuauPrimitiveKind::Entity: {
            if (!lua_isnumber(state, 3)) {
                luaL_typeerror(state, 3, "number");
            }
            const Entity value {
                static_cast<std::uint32_t>(lua_tounsigned(state, 3)),
            };
            auto& current = target.get<Entity>();
            if (current == value) {
                return false;
            }
            current = value;
            return true;
        }
        case LuauPrimitiveKind::Boolean: {
            if (!lua_isboolean(state, 3)) {
                luaL_typeerror(state, 3, "boolean");
            }
            const bool value = lua_toboolean(state, 3) != 0;
            auto& current = target.get<bool>();
            if (current == value) {
                return false;
            }
            current = value;
            return true;
        }
        case LuauPrimitiveKind::Float:
            return assign_direct_number<float>(state, target);
        case LuauPrimitiveKind::Double:
            return assign_direct_number<double>(state, target);
        case LuauPrimitiveKind::SignedChar:
            return assign_direct_number<signed char>(state, target);
        case LuauPrimitiveKind::UnsignedChar:
            return assign_direct_number<unsigned char>(state, target);
        case LuauPrimitiveKind::Short:
            return assign_direct_number<short>(state, target);
        case LuauPrimitiveKind::UnsignedShort:
            return assign_direct_number<unsigned short>(state, target);
        case LuauPrimitiveKind::Int:
            return assign_direct_number<int>(state, target);
        case LuauPrimitiveKind::UnsignedInt:
            return assign_direct_number<unsigned int>(state, target);
        case LuauPrimitiveKind::Long:
            return assign_direct_number<long>(state, target);
        case LuauPrimitiveKind::UnsignedLong:
            return assign_direct_number<unsigned long>(state, target);
        case LuauPrimitiveKind::LongLong:
            return assign_direct_number<long long>(state, target);
        case LuauPrimitiveKind::UnsignedLongLong:
            return assign_direct_number<unsigned long long>(state, target);
        case LuauPrimitiveKind::String: {
            if (!lua_isstring(state, 3)) {
                luaL_typeerror(state, 3, "string");
            }
            std::size_t size = 0;
            const char* text = lua_tolstring(state, 3, &size);
            auto& current = target.get<std::string>();
            if (std::string_view {current} == std::string_view {text, size}) {
                return false;
            }
            current.assign(text, size);
            return true;
        }
        case LuauPrimitiveKind::None:
            luaL_error(state, "unsupported Luau direct property type");
    }
    return false;
}

void direct_property_set(
    lua_State* state,
    const LuauObjectView& object,
    int atom,
    std::uint16_t* cached_slot
) {
    if (object.ref.is_const()) {
        luaL_error(state, "attempt to mutate a read-only ECS borrow");
    }
    auto& path = direct_property_path(state, atom, cached_slot);
    auto target = direct_property_ref(path, object.ref);
    if (!target) {
        raise_message(state, target.error());
        return;
    }
    if (assign_direct_primitive(state, *target, path.primitive_kind)) {
        object.mutation.mark_changed();
    }
}

void borrowed_read_direct_get(
    lua_State* state,
    void* data,
    int atom,
    std::uint16_t* cached_slot,
    int
) {
    auto* object = static_cast<LuauBorrowedObject*>(data);
    if (!luau_borrow_is_valid(object->scope, object->token)) {
        luaL_error(state, "attempt to access an expired ECS borrow");
    }
    direct_property_get(
        state,
        LuauObjectView {
            .ref = object->ref,
            .scope = object->scope,
            .token = object->token,
        },
        atom,
        cached_slot
    );
}

void borrowed_read_direct_set(
    lua_State* state,
    void*,
    int,
    std::uint16_t*,
    int
) {
    luaL_error(state, "attempt to mutate a read-only ECS borrow");
}

void borrowed_write_direct_get(
    lua_State* state,
    void* data,
    int atom,
    std::uint16_t* cached_slot,
    int
) {
    auto* object = static_cast<LuauMutableBorrowedObject*>(data);
    if (!luau_borrow_is_valid(object->scope, object->token)) {
        luaL_error(state, "attempt to access an expired ECS borrow");
    }
    direct_property_get(
        state,
        LuauObjectView {
            .ref = object->ref,
            .scope = object->scope,
            .token = object->token,
            .mutation = object->mutation,
        },
        atom,
        cached_slot
    );
}

void borrowed_write_direct_set(
    lua_State* state,
    void* data,
    int atom,
    std::uint16_t* cached_slot,
    int
) {
    auto* object = static_cast<LuauMutableBorrowedObject*>(data);
    if (!luau_borrow_is_valid(object->scope, object->token)) {
        luaL_error(state, "attempt to access an expired ECS borrow");
    }
    direct_property_set(
        state,
        LuauObjectView {
            .ref = object->ref,
            .scope = object->scope,
            .token = object->token,
            .mutation = object->mutation,
        },
        atom,
        cached_slot
    );
}

void owned_direct_get(
    lua_State* state,
    void* data,
    int atom,
    std::uint16_t* cached_slot,
    int
) {
    auto* object = static_cast<LuauOwnedObject*>(data);
    direct_property_get(
        state,
        LuauObjectView {.ref = object->ref, .owner = &object->owner},
        atom,
        cached_slot
    );
}

void owned_direct_set(
    lua_State* state,
    void* data,
    int atom,
    std::uint16_t* cached_slot,
    int
) {
    auto* object = static_cast<LuauOwnedObject*>(data);
    direct_property_set(
        state,
        LuauObjectView {.ref = object->ref, .owner = &object->owner},
        atom,
        cached_slot
    );
}

int borrowed_index(lua_State* state) {
    auto object = check_luau_object(state, 1);
    if (push_luau_container_index(state, object)) {
        return 1;
    }
    const char* key = luaL_checkstring(state, 2);
    if (push_luau_dynamic_param_member(state, object.ref.type_id(), key)) {
        return 1;
    }
    if (is_luau_state_type(object.ref.type_id())) {
        return raise_message(state, "script state values have no members");
    }
    if (luau_is_commands(object.ref.type_id())) {
        return dispatch_luau_commands_index(state, key);
    }
    if (luau_is_dynamic_world(object.ref.type_id())) {
        return dispatch_luau_world_index(state, key);
    }
    if (luau_is_asset_server(object.ref.type_id()) &&
        push_luau_asset_server_member(state, key)) {
        return 1;
    }
    auto property =
        resolve_property(binding_cache(state), object.ref.type_id(), key);
    if (!property) {
        if (luau_has_method(object.ref, key)) {
            lua_pushstring(state, key);
            lua_pushcclosure(state, luau_invoke_method, key, 1);
            return 1;
        }
        return raise_message(state, property.error());
    }
    auto value = property->property->get(object.ref);
    if (value) {
        push_property_ref(state, *value, object, property->primitive_kind);
        return 1;
    }
    return raise_message(state, value.error().message);
}

int borrowed_newindex(lua_State* state) {
    auto object = check_luau_object(state, 1);
    if (is_luau_state_type(object.ref.type_id())) {
        return raise_message(state, "script state values are immutable");
    }
    if (object.ref.is_const()) {
        luaL_error(state, "attempt to mutate a read-only ECS borrow");
    }
    const char* key = luaL_checkstring(state, 2);
    auto property =
        resolve_property(binding_cache(state), object.ref.type_id(), key);
    if (!property) {
        return raise_message(state, property.error());
    }
    auto primitive_assignment =
        try_set_primitive_property(state, 3, object.ref, *property);
    if (!primitive_assignment) {
        return raise_message(state, primitive_assignment.error().message);
    }
    if (primitive_assignment->handled) {
        if (primitive_assignment->changed) {
            object.mutation.mark_changed();
        }
        return 0;
    }
    auto value =
        luau_value_for_property(state, 3, property->property->type_id());
    if (!value) {
        return raise_message(state, value.error());
    }
    auto changed =
        set_property_if_changed(*property->property, object.ref, value->ref());
    if (!changed) {
        return raise_message(state, changed.error().message);
    }
    if (*changed) {
        object.mutation.mark_changed();
    }
    return 0;
}

int borrowed_equal(lua_State* state) {
    auto lhs = check_luau_object(state, 1);
    auto rhs = check_luau_object(state, 2);
    if (lhs.ref.type_id() != rhs.ref.type_id()) {
        lua_pushboolean(state, false);
        return 1;
    }
    auto type = Registry::instance().try_get_type(lhs.ref.type_id());
    const auto equal =
        type ? type->equals(lhs.ref.const_ptr(), rhs.ref.const_ptr()) :
               Optional<bool> {};
    lua_pushboolean(state, equal.value_or(false));
    return 1;
}

} // namespace

void install_luau_property_metatable(lua_State* state, int metatable) {
    auto* cache = new (lua_newuserdatadtor(
        state,
        sizeof(LuauBindingCache),
        destroy_binding_cache
    )) LuauBindingCache {};
    lua_callbacks(state)->userdata = cache;
    lua_callbacks(state)->useratom = direct_property_atom;
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, borrowed_index, "borrowed.__index", 1);
    lua_setfield(state, metatable, "__index");
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, borrowed_newindex, "borrowed.__newindex", 1);
    lua_setfield(state, metatable, "__newindex");
    lua_pop(state, 1);
    lua_pushcfunction(state, borrowed_equal, "borrowed.__eq");
    lua_setfield(state, metatable, "__eq");
}

void install_luau_property_direct_access(lua_State* state) {
    const bool registered = lua_registeruserdatadirectaccess(
                                state,
                                static_cast<int>(LuauObjectTag::BorrowedRead),
                                borrowed_read_direct_get,
                                borrowed_read_direct_set,
                                nullptr
                            ) != 0 &&
                            lua_registeruserdatadirectaccess(
                                state,
                                static_cast<int>(LuauObjectTag::BorrowedWrite),
                                borrowed_write_direct_get,
                                borrowed_write_direct_set,
                                nullptr
                            ) != 0 &&
                            lua_registeruserdatadirectaccess(
                                state,
                                static_cast<int>(LuauObjectTag::Owned),
                                owned_direct_get,
                                owned_direct_set,
                                nullptr
                            ) != 0;
    if (!registered) {
        luaL_error(state, "failed to register Luau direct property access");
    }
}

Status<std::string> register_luau_property_paths(
    lua_State* state,
    std::span<const LuauPropertyPathDecl> paths
) {
    auto& cache = direct_binding_cache(state);
    for (const auto& declaration : paths) {
        if (const auto existing =
                cache.direct_atoms.find(declaration.atom_name);
            existing != cache.direct_atoms.end()) {
            const auto& path = cache.direct_paths[existing->second];
            if (path.root_type != declaration.root_type ||
                path.leaf_type != declaration.leaf_type ||
                path.properties.size() != declaration.properties.size()) {
                return failure(
                    "Luau direct property atom collision for '" +
                    declaration.atom_name + "'"
                );
            }
            continue;
        }
        if (cache.direct_paths.size() >=
            static_cast<std::size_t>(
                std::numeric_limits<std::int16_t>::max()
            )) {
            return failure(
                std::string {"Luau direct property atom limit exceeded"}
            );
        }

        TypeId current_type = declaration.root_type;
        std::vector<Property*> properties;
        std::vector<Cls*> property_owners;
        std::vector<std::uint64_t> property_revisions;
        properties.reserve(declaration.properties.size());
        property_owners.reserve(declaration.properties.size());
        property_revisions.reserve(declaration.properties.size());
        for (const auto& name : declaration.properties) {
            auto cls = Registry::instance().try_get_cls(current_type);
            if (!cls) {
                return failure(std::move(cls.error().message));
            }
            auto property = cls->try_get_property(name);
            if (!property) {
                return failure(std::move(property.error().message));
            }
            properties.push_back(&*property);
            property_owners.push_back(&*cls);
            property_revisions.push_back(cls->property_revision());
            current_type = property->type_id();
        }
        if (current_type != declaration.leaf_type) {
            return failure(
                std::string {
                    "Luau direct property leaf type changed before module load"
                }
            );
        }
        const auto kind = primitive_kind(current_type);
        if (kind == LuauPrimitiveKind::None) {
            return failure(
                std::string {
                    "Luau direct property leaf is not a supported primitive"
                }
            );
        }

        const auto atom = static_cast<std::int16_t>(cache.direct_paths.size());
        cache.direct_paths.push_back(
            LuauDirectPropertyPath {
                .atom_name = declaration.atom_name,
                .root_type = declaration.root_type,
                .leaf_type = declaration.leaf_type,
                .primitive_kind = kind,
                .property_names = declaration.properties,
                .properties = std::move(properties),
                .property_owners = std::move(property_owners),
                .property_revisions = std::move(property_revisions),
            }
        );
        cache.direct_atoms.emplace(declaration.atom_name, atom);
    }
    return {};
}

Status<std::string> refresh_luau_property_paths(lua_State* state) {
    auto& cache = direct_binding_cache(state);
    for (std::size_t index = 1; index < cache.direct_paths.size(); ++index) {
        auto& path = cache.direct_paths[index];
        bool stale = path.property_owners.size() != path.property_names.size();
        for (std::size_t property = 0;
             !stale && property < path.property_owners.size();
             ++property) {
            stale = path.property_owners[property]->property_revision() !=
                    path.property_revisions[property];
        }
        if (!stale) {
            continue;
        }

        TypeId current_type = path.root_type;
        std::vector<Property*> properties;
        std::vector<Cls*> owners;
        std::vector<std::uint64_t> revisions;
        for (const auto& name : path.property_names) {
            auto cls = Registry::instance().try_get_cls(current_type);
            if (!cls) {
                return failure(std::move(cls.error().message));
            }
            auto property = cls->try_get_property(name);
            if (!property) {
                return failure(std::move(property.error().message));
            }
            properties.push_back(&*property);
            owners.push_back(&*cls);
            revisions.push_back(cls->property_revision());
            current_type = property->type_id();
        }
        if (current_type != path.leaf_type) {
            return failure(
                std::string {
                    "Luau direct property leaf type changed during execution"
                }
            );
        }
        path.properties = std::move(properties);
        path.property_owners = std::move(owners);
        path.property_revisions = std::move(revisions);
        path.offset.reset();
    }
    return {};
}

} // namespace ets::detail
