#include "binding_internal.hpp"
#include "library_scalar.hpp"
#include "refl/callable.hpp"
#include "refl/cls.hpp"
#include "refl/container_adapter.hpp"
#include "refl/enum.hpp"
#include "refl/registry.hpp"
#include "scripting/detail/json_value.hpp"
#include "scripting/detail/reflection_bridge.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <lua.h>
#include <lualib.h>
#include <memory>
#include <nlohmann/json.hpp> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ets::detail {

namespace {

[[nodiscard]] bool is_luau_type_token(lua_State* state, const int index) {
    if (!lua_isuserdata(state, index) || lua_getmetatable(state, index) == 0) {
        return false;
    }
    luaL_getmetatable(state, c_luau_type_token_metatable);
    const bool matches = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return matches;
}

} // namespace

bool luau_reflected_values_equal(Ref lhs, Ref rhs) {
    if (!lhs || !rhs || lhs.type_id() != rhs.type_id()) {
        return false;
    }
    auto type = Registry::instance().try_get_type(lhs.type_id());
    return type &&
           type->equals(lhs.const_ptr(), rhs.const_ptr()).value_or(false);
}

TypeId
check_luau_type_token(lua_State* state, int index, std::string_view context) {
    auto* token = static_cast<LuauTypeToken*>(
        luaL_checkudata(state, index, c_luau_type_token_metatable)
    );
    if (token == nullptr || !token->type) {
        luaL_error(
            state,
            "%.*s expects a reflected type",
            static_cast<int>(context.size()),
            context.data()
        );
        return {};
    }
    return token->type;
}

namespace {

int raise_message(lua_State* state, const std::string& message) {
    luaL_error(state, "%s", message.c_str());
    return 0;
}

Result<Val, std::string> argument_value(lua_State* state, int index) {
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            return make_val<bool>(lua_toboolean(state, index) != 0);
        case LUA_TNUMBER: {
            const double number = lua_tonumber(state, index);
            if (std::trunc(number) == number &&
                number >=
                    static_cast<double>(std::numeric_limits<int>::min()) &&
                number <=
                    static_cast<double>(std::numeric_limits<int>::max())) {
                return make_val<int>(static_cast<int>(number));
            }
            return make_val<float>(static_cast<float>(number));
        }
        case LUA_TSTRING: {
            std::size_t size = 0;
            const char* text = lua_tolstring(state, index, &size);
            return make_val<std::string>(text, size);
        }
        default:
            return failure(
                "unsupported reflected method argument at index " +
                std::to_string(index)
            );
    }
}

void push_owned_value(lua_State* state, Val value) {
    if (!value) {
        lua_pushnil(state);
        return;
    }
    if (value.type_id() == type_id<nlohmann::json>()) {
        push_json(state, value.get<nlohmann::json>());
        return;
    }
    if (push_luau_primitive(state, value.ref())) {
        return;
    }
    auto owner = std::make_shared<Val>(std::move(value));
    LuauObjectView parent {
        .ref = owner->ref(),
        .owner = &owner,
    };
    push_luau_ref(state, parent.ref, parent);
}

struct MutationSnapshot {
    Ref current;
    LuauMutationContext mutation;
    Optional<Val> previous;
};

void capture_mutation_snapshot(
    const LuauObjectView& object,
    std::vector<MutationSnapshot>& snapshots
) {
    if (!object.mutation) {
        return;
    }
    MutationSnapshot snapshot {
        .current = object.ref,
        .mutation = object.mutation,
    };
    if (auto copied = Val::copy(object.ref)) {
        snapshot.previous = std::move(*copied);
    }
    snapshots.push_back(std::move(snapshot));
}

void apply_mutation_snapshots(const std::vector<MutationSnapshot>& snapshots) {
    for (const auto& snapshot : snapshots) {
        if (!snapshot.previous || !luau_reflected_values_equal(
                                      snapshot.previous->ref(),
                                      snapshot.current
                                  )) {
            snapshot.mutation.mark_changed();
        }
    }
}

int push_return_item(
    lua_State* state,
    ReturnItem& item,
    const LuauObjectView& instance
) {
    if (item.is_ref()) {
        push_luau_ref(state, item.ref(), instance);
    } else {
        push_owned_value(state, std::move(item.value()));
    }
    return 1;
}

int push_return_item(
    lua_State* state,
    const ReturnItem& item,
    const LuauObjectView& instance
) {
    if (item.is_ref()) {
        push_luau_ref(state, item.ref(), instance);
    } else {
        push_owned_value(state, item.value());
    }
    return 1;
}

int push_invoke_result(
    lua_State* state,
    InvokeResult result,
    const LuauObjectView& instance
) {
    if (!result) {
        auto& error = result.error();
        if (error.kind == InvokeFailure::Kind::ReturnedError) {
            lua_pushnil(state);
            push_owned_value(state, std::move(error.error));
            return 2;
        }
        return raise_message(state, error.message);
    }

    ReturnValue& value = *result;
    switch (value.kind()) {
        case ReturnValue::Kind::Void:
            return 0;
        case ReturnValue::Kind::Status:
            lua_pushboolean(state, true);
            return 1;
        case ReturnValue::Kind::One:
            return push_return_item(state, value.item(), instance);
        case ReturnValue::Kind::Many: {
            int count = 0;
            for (const ReturnItem& item : value.items()) {
                count += push_return_item(state, item, instance);
            }
            return count;
        }
    }
    return 0;
}

int invoke_static_method(lua_State* state) {
    const char* name = lua_tostring(state, lua_upvalueindex(1));
    const TypeId type = check_luau_type_token(
        state,
        lua_upvalueindex(2),
        "reflected static method"
    );
    std::vector<Val> owned_arguments;
    owned_arguments.reserve(static_cast<std::size_t>(lua_gettop(state)));
    std::vector<Ref> refs;
    refs.reserve(static_cast<std::size_t>(lua_gettop(state)));
    std::vector<MutationSnapshot> mutation_snapshots;
    for (int index = 1; index <= lua_gettop(state); ++index) {
        if (is_luau_type_token(state, index)) {
            owned_arguments.push_back(
                make_val<TypeId>(
                    check_luau_type_token(state, index, "reflected method")
                )
            );
            refs.push_back(owned_arguments.back().ref());
            continue;
        }
        if (lua_isuserdata(state, index)) {
            auto object = check_luau_object(state, index);
            refs.push_back(object.ref);
            capture_mutation_snapshot(object, mutation_snapshots);
            continue;
        }
        auto argument = argument_value(state, index);
        if (!argument) {
            return raise_message(state, argument.error());
        }
        owned_arguments.push_back(std::move(*argument));
        refs.push_back(owned_arguments.back().ref());
    }

    auto result = luau_invoke_static_method(type, name, refs);
    apply_mutation_snapshots(mutation_snapshots);
    return push_invoke_result(state, std::move(result), LuauObjectView {});
}

int construct_type(lua_State* state, TypeId type, int first_argument) {
    const int argument_count = lua_gettop(state) - first_argument + 1;
    if (argument_count == 1 && lua_istable(state, first_argument)) {
        auto value = luau_value_for_type(state, first_argument, type);
        if (!value) {
            return raise_message(state, value.error());
        }
        push_owned_value(state, std::move(*value));
        return 1;
    }

    std::vector<Val> owned_arguments;
    owned_arguments.reserve(static_cast<std::size_t>(argument_count));
    std::vector<Ref> arguments;
    arguments.reserve(static_cast<std::size_t>(argument_count));
    for (int index = first_argument; index <= lua_gettop(state); ++index) {
        if (is_luau_type_token(state, index)) {
            owned_arguments.push_back(
                make_val<TypeId>(
                    check_luau_type_token(state, index, "reflected constructor")
                )
            );
            arguments.push_back(owned_arguments.back().ref());
            continue;
        }
        if (lua_isuserdata(state, index)) {
            arguments.push_back(check_luau_object(state, index).ref);
            continue;
        }
        auto argument = argument_value(state, index);
        if (!argument) {
            return raise_message(state, argument.error());
        }
        owned_arguments.push_back(std::move(*argument));
        arguments.push_back(owned_arguments.back().ref());
    }
    auto value = luau_construct(type, arguments);
    if (!value) {
        return raise_message(state, value.error().message);
    }
    push_owned_value(state, std::move(*value));
    return 1;
}

int type_new(lua_State* state) {
    const TypeId type = check_luau_type_token(
        state,
        lua_upvalueindex(1),
        "reflected constructor"
    );
    return construct_type(state, type, 1);
}

} // namespace

int invoke_luau_library(
    lua_State* state,
    const Method& method,
    const std::shared_ptr<Val>& owner
) {
    try {
        const auto& params = method.params();
        if (lua_gettop(state) > static_cast<int>(params.size())) {
            throw std::invalid_argument("Too many arguments");
        }
        std::vector<Val> values;
        std::vector<Ref> args;
        std::vector<MutationSnapshot> mutations;
        values.reserve(params.size());
        args.reserve(params.size() + 1);
        if (!method.is_static()) {
            if (!owner) {
                throw std::invalid_argument(
                    "Library instance requires a default or services "
                    "constructor"
                );
            }
            args.push_back(owner->ref());
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            const auto expected = params[i].type_id();
            const auto container =
                Registry::instance().try_get_container_adapter(expected);
            if ((!container || container->kind() != ContainerKind::Optional) &&
                lua_isuserdata(state, index) && expected != type_id<TypeId>() &&
                expected != type_id<Ref>() && expected != type_id<Val>() &&
                expected != type_id<nlohmann::json>()) {
                auto object = check_luau_object(state, index);
                args.push_back(object.ref);
                capture_mutation_snapshot(object, mutations);
                continue;
            }
            const auto qualification = params[i].type();
            if ((qualification.is_reference() && !qualification.is_const()) ||
                qualification.is_rvalue_reference()) {
                throw std::invalid_argument(
                    "Mutable references require a reflected object argument"
                );
            }
            auto value = luau_value_for_type(state, index, expected);
            if (!value) {
                throw std::invalid_argument(
                    "argument " + std::to_string(index) + ": " + value.error()
                );
            }
            values.push_back(std::move(*value));
            args.push_back(values.back().ref());
        }
        auto result = method.invoke_variadic(args);
        apply_mutation_snapshots(mutations);
        if (result && result->is_one() && result->item().is_value() &&
            method.return_type().is_const()) {
            push_luau_readonly_value(state, std::move(result->item().value()));
            return 1;
        }
        return push_invoke_result(
            state,
            std::move(result),
            LuauObjectView {
                .ref = owner ? owner->ref() : Ref {},
                .owner = owner ? &owner : nullptr
            }
        );
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    lua_error(state);
}

int luau_type_token_call(lua_State* state) {
    const TypeId type =
        check_luau_type_token(state, 1, "reflected constructor");
    return construct_type(state, type, 2);
}

Result<Val, std::string>
luau_value_for_property(lua_State* state, int index, TypeId expected) {
    if (lua_isuserdata(state, index)) {
        const auto object = check_luau_object(state, index);
        auto copied = Val::copy(object.ref);
        if (copied) {
            return std::move(*copied);
        }
        return failure(std::move(copied.error().message));
    }
    return luau_value_for_type(state, index, expected);
}

Result<Val, std::string>
luau_value_for_type(lua_State* state, int index, TypeId expected) {
    if (expected == type_id<nlohmann::json>()) {
        try {
            std::size_t nodes = 0;
            return make_val<nlohmann::json>(
                luau_json(state, index, "value", 0, nodes, true)
            );
        } catch (const std::exception& error) {
            return failure(std::string(error.what()));
        }
    }
    if (expected == type_id<Ref>()) {
        if (!lua_isuserdata(state, index)) {
            return failure(std::string("Expected a reflected object"));
        }
        return make_val<Ref>(check_luau_object(state, index).ref);
    }
    if (expected == type_id<Val>()) {
        auto value = copy_luau_reflected_value(state, index, "value");
        if (!value) {
            return failure(value.error());
        }
        return make_val<Val>(std::move(*value));
    }
#define ETS_LUAU_SCALAR(T)                                              \
    if (expected == type_id<T>()) {                                     \
        try {                                                           \
            return make_val<T>(LuauScalarCodec<T>::read(state, index)); \
        } catch (const std::exception& error) {                         \
            return failure(std::string(error.what()));                  \
        }                                                               \
    }
    ETS_LUAU_SCALAR(bool)
    ETS_LUAU_SCALAR(char)
    ETS_LUAU_SCALAR(char8_t)
    ETS_LUAU_SCALAR(char16_t)
    ETS_LUAU_SCALAR(char32_t)
    ETS_LUAU_SCALAR(signed char)
    ETS_LUAU_SCALAR(unsigned char)
    ETS_LUAU_SCALAR(short)
    ETS_LUAU_SCALAR(unsigned short)
    ETS_LUAU_SCALAR(int)
    ETS_LUAU_SCALAR(unsigned int)
    ETS_LUAU_SCALAR(long)
    ETS_LUAU_SCALAR(unsigned long)
    ETS_LUAU_SCALAR(long long)
    ETS_LUAU_SCALAR(unsigned long long)
    ETS_LUAU_SCALAR(float)
    ETS_LUAU_SCALAR(double)
    ETS_LUAU_SCALAR(long double)
    ETS_LUAU_SCALAR(std::string)
    ETS_LUAU_SCALAR(std::string_view)
#undef ETS_LUAU_SCALAR
    auto adapter = Registry::instance().try_get_container_adapter(expected);
    if (adapter && adapter->kind() == ContainerKind::Optional) {
        auto type = Registry::instance().try_get_type(expected);
        if (!type) {
            return failure(type.error().message);
        }
        Val optional = Val::default_construct(*type);
        if (lua_isnoneornil(state, index)) {
            return optional;
        }
        auto* indexed = adapter->indexed();
        if (indexed == nullptr) {
            return failure(
                std::string {
                    "Reflected optional does not support indexed access"
                }
            );
        }
        auto value = luau_value_for_type(state, index, indexed->element_type());
        if (!value) {
            return failure(std::move(value.error()));
        }
        auto appended = indexed->append(optional.ref(), value->ref());
        if (!appended) {
            return failure(std::move(appended.error().message));
        }
        return optional;
    }
    if (lua_istable(state, index)) {
        struct ConversionScope {
            lua_State* state;
            int top;
            unsigned& depth;
            ~ConversionScope() {
                lua_settop(state, top);
                --depth;
            }
        };
        static thread_local unsigned depth = 0;
        if (depth >= 64 || !lua_checkstack(state, 8)) {
            return failure(std::string("Table conversion depth exceeded"));
        }
        ++depth;
        ConversionScope scope {state, lua_gettop(state), depth};
        index = lua_absindex(state, index);
        if (lua_getmetatable(state, index)) {
            return failure(std::string("Expected a plain table"));
        }
        auto type = Registry::instance().try_get_type(expected);
        if (!type || !type->default_constructible()) {
            return failure(
                std::string("Type cannot be initialized from a table")
            );
        }
        Val value = Val::default_construct(*type);
        if (adapter) {
            if (auto* indexed = adapter->indexed()) {
                const auto count =
                    static_cast<std::size_t>(lua_objlen(state, index));
                if (count > 100000) {
                    return failure(std::string("Array is too large"));
                }
                std::size_t keys = 0;
                lua_pushnil(state);
                while (lua_next(state, index)) {
                    const double key = lua_type(state, -2) == LUA_TNUMBER ?
                                           lua_tonumber(state, -2) :
                                           0;
                    if (key < 1 || key > static_cast<double>(count) ||
                        std::floor(key) != key) {
                        return failure(std::string("Expected a dense array"));
                    }
                    ++keys;
                    lua_pop(state, 1);
                }
                if (keys != count) {
                    return failure(std::string("Expected a dense array"));
                }
                if (indexed->fixed_size()) {
                    auto size = indexed->size(value.ref());
                    if (!size || *size != count) {
                        return failure(std::string("Wrong fixed array size"));
                    }
                }
                for (std::size_t i = 0; i < count; ++i) {
                    auto element_type = indexed->element_type(value.ref(), i);
                    if (!element_type) {
                        return failure(element_type.error().message);
                    }
                    lua_rawgeti(state, index, static_cast<int>(i + 1));
                    auto element =
                        luau_value_for_type(state, -1, *element_type);
                    if (!element) {
                        return failure(
                            "[" + std::to_string(i + 1) +
                            "]: " + element.error()
                        );
                    }
                    auto assigned =
                        indexed->fixed_size() ?
                            indexed->assign(value.ref(), i, element->ref()) :
                            indexed->append(value.ref(), element->ref());
                    if (!assigned) {
                        return failure(assigned.error().message);
                    }
                    lua_pop(state, 1);
                }
                return value;
            }
            if (auto* associative = adapter->associative();
                associative && associative->has_mapped_value()) {
                lua_pushnil(state);
                std::size_t count = 0;
                while (lua_next(state, index)) {
                    if (++count > 100000) {
                        return failure(std::string("Map is too large"));
                    }
                    auto key =
                        luau_value_for_type(state, -2, associative->key_type());
                    auto element = luau_value_for_type(
                        state,
                        -1,
                        associative->mapped_type()
                    );
                    if (!key) {
                        return failure(key.error());
                    }
                    if (!element) {
                        return failure(element.error());
                    }
                    auto inserted = associative->insert(
                        value.ref(),
                        {key->ref(), element->ref()}
                    );
                    if (!inserted) {
                        return failure(inserted.error().message);
                    }
                    lua_pop(state, 1);
                }
                return value;
            }
            return failure(
                std::string("Unsupported table container conversion")
            );
        }
        auto cls = Registry::instance().try_get_cls(expected);
        if (!cls) {
            return failure(cls.error().message);
        }
        lua_pushnil(state);
        while (lua_next(state, index)) {
            if (lua_type(state, -2) != LUA_TSTRING) {
                return failure(std::string("Field names must be strings"));
            }
            const std::string name = lua_tostring(state, -2);
            auto property = cls->try_get_property(name);
            if (!property) {
                return failure(property.error().message);
            }
            auto field = luau_value_for_type(state, -1, property->type_id());
            if (!field) {
                return failure(name + ": " + field.error());
            }
            auto assigned = luau_set_property(value.ref(), name, field->ref());
            if (!assigned) {
                return failure(assigned.error().message);
            }
            lua_pop(state, 1);
        }
        return value;
    }
    if (expected == type_id<Entity>() && lua_isnumber(state, index)) {
        return make_val<Entity>(Entity {
            static_cast<std::uint32_t>(lua_tounsigned(state, index)),
        });
    }
    if (expected == type_id<TypeId>() && is_luau_type_token(state, index)) {
        return make_val<TypeId>(
            check_luau_type_token(state, index, "reflected value")
        );
    }
    if (lua_isuserdata(state, index)) {
        auto object = check_luau_object(state, index);
        if (object.ref.type_id() == expected) {
            auto copied = Val::copy(object.ref);
            if (copied) {
                return std::move(*copied);
            }
            return failure(copied.error().message);
        }
    }
    return failure(
        "value is incompatible with reflected type '" + type_name(expected) +
        "'"
    );
}

int luau_type_token_index(lua_State* state) {
    const TypeId type =
        check_luau_type_token(state, 1, "reflected type access");
    const char* key = luaL_checkstring(state, 2);
    if (std::string_view {key} == "new") {
        lua_pushvalue(state, 1);
        lua_pushcclosure(state, type_new, "type.new", 1);
        return 1;
    }
    if (std::string_view {key} == "__ets_type_id") {
        lua_pushinteger(state, static_cast<lua_Integer>(type.id()));
        return 1;
    }
    if (std::string_view {key} == "__ets_type_name") {
        const auto reflected_type = Registry::instance().try_get_type(type);
        if (!reflected_type) {
            return raise_message(state, reflected_type.error().message);
        }
        lua_pushlstring(
            state,
            reflected_type->name().data(),
            reflected_type->name().size()
        );
        return 1;
    }
    if (auto enm = Registry::instance().try_get_enum(type)) {
        const auto enumerator = enm->enumerators().find(key);
        if (enumerator != enm->enumerators().end()) {
            push_owned_value(state, enm->make_val(enumerator->second));
            return 1;
        }
    }
    if (luau_has_static_method(type, key)) {
        lua_pushstring(state, key);
        lua_pushvalue(state, 1);
        lua_pushcclosure(state, invoke_static_method, key, 2);
        return 1;
    }
    return raise_message(
        state,
        "unknown reflected type member '" + std::string {key} + "'"
    );
}

int luau_invoke_method(lua_State* state) {
    const char* name = lua_tostring(state, lua_upvalueindex(1));
    auto instance = check_luau_object(state, 1);
    std::vector<MutationSnapshot> mutation_snapshots;
    capture_mutation_snapshot(instance, mutation_snapshots);
    const int argument_count = lua_gettop(state) - 1;
    std::vector<Val> owned_arguments;
    owned_arguments.reserve(static_cast<std::size_t>(argument_count));
    std::vector<Ref> refs;
    refs.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 2; index <= lua_gettop(state); ++index) {
        if (is_luau_type_token(state, index)) {
            owned_arguments.push_back(
                make_val<TypeId>(
                    check_luau_type_token(state, index, "reflected method")
                )
            );
            refs.push_back(owned_arguments.back().ref());
            continue;
        }
        if (lua_isuserdata(state, index)) {
            auto object = check_luau_object(state, index);
            refs.push_back(object.ref);
            capture_mutation_snapshot(object, mutation_snapshots);
            continue;
        }
        auto argument = argument_value(state, index);
        if (!argument) {
            return raise_message(state, argument.error());
        }
        owned_arguments.push_back(std::move(*argument));
        refs.push_back(owned_arguments.back().ref());
    }

    auto result = luau_invoke_method(instance.ref, name, refs);
    apply_mutation_snapshots(mutation_snapshots);
    return push_invoke_result(state, std::move(result), instance);
}

Result<Val, std::string> copy_luau_reflected_value(
    lua_State* state,
    int index,
    std::string_view context
) {
    if (lua_isuserdata(state, index)) {
        auto object = check_luau_borrowed_ref(state, index);
        auto copied = Val::copy(object.ref);
        if (copied) {
            return std::move(*copied);
        }
        return failure(copied.error().message);
    }
    auto value = argument_value(state, index);
    if (value) {
        return value;
    }
    return failure(
        std::string {context} + " expects a reflected value: " + value.error()
    );
}

void push_luau_owned_value(lua_State* state, Val value) {
    push_owned_value(state, std::move(value));
}

void push_luau_type_token(lua_State* state, TypeId type) {
    auto* token = new (lua_newuserdata(state, sizeof(LuauTypeToken)))
        LuauTypeToken {.type = type};
    static_cast<void>(token);
    luaL_getmetatable(state, c_luau_type_token_metatable);
    lua_setmetatable(state, -2);
}

} // namespace ets::detail
