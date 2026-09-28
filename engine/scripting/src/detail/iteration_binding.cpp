#include "binding_internal.hpp"
#include "ecs/dynamic/events.hpp"
#include "ecs/dynamic/query.hpp"
#include "ecs/dynamic/removed_components.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <lua.h>
#include <lualib.h>

namespace ets::detail {
namespace {

constexpr std::size_t c_max_reusable_query_fields = 32;
constexpr std::size_t c_query_chunk_size = 32;

struct LuauQueryIterator {
    DynamicQuery* query {nullptr};
    DynamicQueryCursor cursor;
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
};

struct LuauReusableQueryIterator {
    LuauQueryIterator iterator;
    std::array<void*, c_max_reusable_query_fields> reusable_objects {};
    std::array<std::uint8_t, c_max_reusable_query_fields> reusable_upvalues {};
    std::uint32_t reusable_write_fields {0};
};

struct LuauChunkQueryIterator {
    LuauQueryIterator iterator;
    std::size_t previous_count {0};
    std::uint32_t reusable_fields {0};
    std::uint32_t reusable_write_fields {0};
};

struct LuauRemovedComponentsIterator {
    DynamicRemovedComponents* removed {nullptr};
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
};

struct LuauDynamicEventIterator {
    DynamicEventParam* reader {nullptr};
    LuauBorrowScope* scope {nullptr};
    LuauBorrowToken token;
};

void update_reusable_query_field(
    LuauReusableQueryIterator& iterator,
    const DynamicQueryFieldBorrow& field,
    std::size_t index
) {
    const bool writable =
        (iterator.reusable_write_fields & (std::uint32_t {1} << index)) != 0;
    if (writable) {
        auto* object = static_cast<LuauMutableBorrowedObject*>(
            iterator.reusable_objects[index]
        );
        object->ref = field.value;
        object->mutation = LuauMutationContext {
            .ticks = field.ticks,
            .tick = field.change_tick,
        };
        return;
    }

    auto* object =
        static_cast<LuauBorrowedObject*>(iterator.reusable_objects[index]);
    object->ref = field.value;
}

void*& chunk_query_object(
    LuauChunkQueryIterator& iterator,
    std::size_t field_index,
    std::size_t row
) {
    // The userdata allocation stores a dense field-by-row pointer matrix
    // immediately after the fixed iterator header.
    auto* objects = reinterpret_cast<void**>(&iterator + 1);
    return objects[field_index * c_query_chunk_size + row];
}

void update_chunk_query_field(
    lua_State* state,
    LuauChunkQueryIterator& iterator,
    const DynamicQueryFieldBorrow& field,
    std::size_t field_index,
    std::size_t row
) {
    const bool writable = (iterator.reusable_write_fields &
                           (std::uint32_t {1} << field_index)) != 0;
    void*& storage = chunk_query_object(iterator, field_index, row);
    if (storage == nullptr) {
        if (writable) {
            storage = new (lua_newuserdatataggedwithmetatable(
                state,
                sizeof(LuauMutableBorrowedObject),
                static_cast<int>(LuauObjectTag::BorrowedWrite)
            )) LuauMutableBorrowedObject {
                .scope = iterator.iterator.scope,
                .token = iterator.iterator.token,
            };
        } else {
            storage = new (lua_newuserdatataggedwithmetatable(
                state,
                sizeof(LuauBorrowedObject),
                static_cast<int>(LuauObjectTag::BorrowedRead)
            )) LuauBorrowedObject {
                .scope = iterator.iterator.scope,
                .token = iterator.iterator.token,
            };
        }
        lua_rawseti(
            state,
            lua_upvalueindex(static_cast<int>(field_index + 2)),
            static_cast<int>(row + 1)
        );
    }
    if (writable) {
        auto* object = static_cast<LuauMutableBorrowedObject*>(storage);
        object->ref = field.value;
        object->mutation = LuauMutationContext {
            .ticks = field.ticks,
            .tick = field.change_tick,
        };
        return;
    }

    auto* object = static_cast<LuauBorrowedObject*>(storage);
    object->ref = field.value;
}

void push_query_field(
    lua_State* state,
    LuauReusableQueryIterator& iterator,
    const DynamicQueryFieldBorrow& field,
    std::size_t index
) {
    const auto reusable_upvalue = index < iterator.reusable_upvalues.size() ?
                                      iterator.reusable_upvalues[index] :
                                      std::uint8_t {0};
    if (reusable_upvalue == 0) {
        push_luau_borrowed_value(
            state,
            field.value,
            *iterator.iterator.scope,
            iterator.iterator.token,
            LuauMutationContext {
                .ticks = field.ticks,
                .tick = field.change_tick,
            }
        );
        return;
    }

    update_reusable_query_field(iterator, field, index);
    lua_pushvalue(state, lua_upvalueindex(reusable_upvalue));
}

int capture_reusable_query_fields(
    lua_State* state,
    LuauReusableQueryIterator& iterator,
    std::uint32_t reusable_fields
) {
    int upvalue_count = 1;
    const auto& fields = iterator.iterator.query->fields();
    const std::size_t count =
        std::min(fields.size(), iterator.reusable_objects.size());
    for (std::size_t index = 0; index < count; ++index) {
        const bool reusable =
            (reusable_fields & (std::uint32_t {1} << index)) != 0;
        if (!reusable ||
            fields[index].kind != DynamicQueryFieldKind::Component) {
            continue;
        }

        ++upvalue_count;
        iterator.reusable_upvalues[index] =
            static_cast<std::uint8_t>(upvalue_count);
        if (fields[index].access == DynamicParamAccess::Write) {
            iterator.reusable_write_fields |= std::uint32_t {1} << index;
            auto* object = new (lua_newuserdatataggedwithmetatable(
                state,
                sizeof(LuauMutableBorrowedObject),
                static_cast<int>(LuauObjectTag::BorrowedWrite)
            )) LuauMutableBorrowedObject {
                .scope = iterator.iterator.scope,
                .token = iterator.iterator.token,
            };
            iterator.reusable_objects[index] = object;
        } else {
            auto* object = new (lua_newuserdatataggedwithmetatable(
                state,
                sizeof(LuauBorrowedObject),
                static_cast<int>(LuauObjectTag::BorrowedRead)
            )) LuauBorrowedObject {
                .scope = iterator.iterator.scope,
                .token = iterator.iterator.token,
            };
            iterator.reusable_objects[index] = object;
        }
    }
    return upvalue_count;
}

int query_next(lua_State* state) {
    auto* iterator = static_cast<LuauQueryIterator*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (iterator == nullptr ||
        !luau_borrow_is_valid(iterator->scope, iterator->token)) {
        luaL_error(state, "attempt to iterate an expired ECS borrow");
    }

    DynamicQueryRow row;
    if (!iterator->query->next(iterator->cursor, row)) {
        return 0;
    }
    const auto& fields = iterator->query->fields();
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto field = iterator->query->field_untracked(row, index);
        push_luau_borrowed_value(
            state,
            field.value,
            *iterator->scope,
            iterator->token,
            LuauMutationContext {
                .ticks = field.ticks,
                .tick = field.change_tick,
            }
        );
    }
    return static_cast<int>(fields.size());
}

int reusable_query_next(lua_State* state) {
    auto* reusable = static_cast<LuauReusableQueryIterator*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (reusable == nullptr) {
        luaL_error(state, "reusable Query iterator is unavailable");
    }
    auto& iterator = reusable->iterator;
    if (!luau_borrow_is_valid(iterator.scope, iterator.token)) {
        luaL_error(state, "attempt to iterate an expired ECS borrow");
    }

    DynamicQueryRow row;
    if (!iterator.query->next(iterator.cursor, row)) {
        return 0;
    }
    const auto& fields = iterator.query->fields();
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto field = iterator.query->field_untracked(row, index);
        push_query_field(state, *reusable, field, index);
    }
    return static_cast<int>(fields.size());
}

int chunk_query_refill(lua_State* state) {
    auto* chunk = static_cast<LuauChunkQueryIterator*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (chunk == nullptr) {
        luaL_error(state, "chunked Query iterator is unavailable");
    }
    auto& iterator = chunk->iterator;
    if (!luau_borrow_is_valid(iterator.scope, iterator.token)) {
        luaL_error(state, "attempt to iterate an expired ECS borrow");
    }

    const auto& fields = iterator.query->fields();
    std::size_t count = 0;
    DynamicQueryRow row;
    while (count < c_query_chunk_size &&
           iterator.query->next(iterator.cursor, row)) {
        for (std::size_t field_index = 0; field_index < fields.size();
             ++field_index) {
            const auto field =
                iterator.query->field_untracked(row, field_index);
            const bool reusable = field_index < c_max_reusable_query_fields &&
                                  (chunk->reusable_fields &
                                   (std::uint32_t {1} << field_index)) != 0;
            if (reusable) {
                update_chunk_query_field(
                    state,
                    *chunk,
                    field,
                    field_index,
                    count
                );
                continue;
            }

            push_luau_borrowed_value(
                state,
                field.value,
                *iterator.scope,
                iterator.token,
                LuauMutationContext {
                    .ticks = field.ticks,
                    .tick = field.change_tick,
                }
            );
            lua_rawseti(
                state,
                lua_upvalueindex(static_cast<int>(field_index + 2)),
                static_cast<int>(count + 1)
            );
        }
        ++count;
    }

    for (std::size_t field_index = 0; field_index < fields.size();
         ++field_index) {
        const bool reusable =
            field_index < c_max_reusable_query_fields &&
            (chunk->reusable_fields & (std::uint32_t {1} << field_index)) != 0;
        if (reusable) {
            continue;
        }
        for (std::size_t row_index = count; row_index < chunk->previous_count;
             ++row_index) {
            lua_pushnil(state);
            lua_rawseti(
                state,
                lua_upvalueindex(static_cast<int>(field_index + 2)),
                static_cast<int>(row_index + 1)
            );
        }
    }
    chunk->previous_count = count;
    lua_pushunsigned(state, static_cast<unsigned int>(count));
    return 1;
}

int removed_components_iterator_next(lua_State* state) {
    auto* iterator = static_cast<LuauRemovedComponentsIterator*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (iterator == nullptr ||
        !luau_borrow_is_valid(iterator->scope, iterator->token)) {
        luaL_error(state, "attempt to iterate an expired ECS borrow");
    }
    auto entity = iterator->removed->next();
    if (!entity) {
        return 0;
    }
    lua_pushunsigned(state, entity->value);
    return 1;
}

int dynamic_event_iterator_next(lua_State* state) {
    auto* iterator = static_cast<LuauDynamicEventIterator*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    if (iterator == nullptr ||
        !luau_borrow_is_valid(iterator->scope, iterator->token)) {
        luaL_error(state, "attempt to iterate an expired ECS borrow");
    }
    auto event = iterator->reader->next();
    if (!event) {
        return 0;
    }
    push_luau_borrowed_value(state, *event, *iterator->scope, iterator->token);
    return 1;
}

} // namespace

int luau_borrowed_iter(lua_State* state) {
    auto object = check_luau_object(state, 1);
    if (push_luau_container_iterator(state, object)) {
        return 1;
    }
    auto* query = object.ref.try_get<DynamicQuery>();
    if (query != nullptr) {
        auto* iterator = new (lua_newuserdata(state, sizeof(LuauQueryIterator)))
            LuauQueryIterator {
                .query = query,
                .scope = object.scope,
                .token = object.token,
            };
        static_cast<void>(iterator);
        lua_pushcclosure(state, query_next, "DynamicQuery.next", 1);
        return 1;
    }
    auto* removed = object.ref.try_get<DynamicRemovedComponents>();
    if (removed != nullptr) {
        auto* iterator =
            new (lua_newuserdata(state, sizeof(LuauRemovedComponentsIterator)))
                LuauRemovedComponentsIterator {
                    .removed = removed,
                    .scope = object.scope,
                    .token = object.token,
                };
        static_cast<void>(iterator);
        lua_pushcclosure(
            state,
            removed_components_iterator_next,
            "RemovedComponents.next",
            1
        );
        return 1;
    }
    auto* event_reader = object.ref.try_get<DynamicEventParam>();
    if (event_reader != nullptr &&
        event_reader->kind() != DynamicEventParamKind::Writer) {
        auto* iterator =
            new (lua_newuserdata(state, sizeof(LuauDynamicEventIterator)))
                LuauDynamicEventIterator {
                    .reader = event_reader,
                    .scope = object.scope,
                    .token = object.token,
                };
        static_cast<void>(iterator);
        lua_pushcclosure(
            state,
            dynamic_event_iterator_next,
            "EventReader.next",
            1
        );
        return 1;
    }
    luaL_error(state, "value is not iterable");
}

int luau_reusable_query(lua_State* state) {
    auto object = check_luau_object(state, 1);
    auto* query = object.ref.try_get<DynamicQuery>();
    if (query == nullptr || object.scope == nullptr) {
        luaL_typeerror(state, 1, "borrowed Query");
        return 0;
    }
    const auto reusable_fields =
        static_cast<std::uint32_t>(luaL_checkunsigned(state, 2));
    auto* iterator =
        new (lua_newuserdata(state, sizeof(LuauReusableQueryIterator)))
            LuauReusableQueryIterator {
                .iterator = LuauQueryIterator {
                    .query = query,
                    .scope = object.scope,
                    .token = object.token,
                },
            };
    const int upvalue_count =
        capture_reusable_query_fields(state, *iterator, reusable_fields);
    lua_pushcclosure(
        state,
        reusable_query_next,
        "DynamicQuery.reusable_next",
        upvalue_count
    );
    return 1;
}

int luau_chunk_query(lua_State* state) {
    auto object = check_luau_object(state, 1);
    auto* query = object.ref.try_get<DynamicQuery>();
    if (query == nullptr || object.scope == nullptr) {
        luaL_typeerror(state, 1, "borrowed Query");
        return 0;
    }
    const auto& fields = query->fields();
    if (fields.empty() || fields.size() > c_max_reusable_query_fields) {
        luaL_error(state, "chunked Query field count is unsupported");
        return 0;
    }

    const auto reusable_fields =
        static_cast<std::uint32_t>(luaL_checkunsigned(state, 2));
    std::uint32_t reusable_component_fields = 0;
    for (std::size_t field_index = 0; field_index < fields.size();
         ++field_index) {
        if ((reusable_fields & (std::uint32_t {1} << field_index)) != 0 &&
            fields[field_index].kind == DynamicQueryFieldKind::Component) {
            reusable_component_fields |= std::uint32_t {1} << field_index;
        }
    }
    const std::size_t object_count = fields.size() * c_query_chunk_size;
    const std::size_t allocation_size =
        sizeof(LuauChunkQueryIterator) + object_count * sizeof(void*);
    auto* chunk =
        new (lua_newuserdata(state, allocation_size)) LuauChunkQueryIterator {
            .iterator =
                LuauQueryIterator {
                    .query = query,
                    .scope = object.scope,
                    .token = object.token,
                },
            .reusable_fields = reusable_component_fields,
        };
    auto* objects = reinterpret_cast<void**>(chunk + 1);
    std::fill(objects, objects + object_count, nullptr);
    const int chunk_index = lua_absindex(state, -1);

    for (std::size_t field_index = 0; field_index < fields.size();
         ++field_index) {
        lua_createtable(state, static_cast<int>(c_query_chunk_size), 0);
        const bool reusable = (reusable_component_fields &
                               (std::uint32_t {1} << field_index)) != 0;
        if (reusable &&
            fields[field_index].access == DynamicParamAccess::Write) {
            chunk->reusable_write_fields |= std::uint32_t {1} << field_index;
        }
    }

    lua_pushvalue(state, chunk_index);
    for (std::size_t field_index = 0; field_index < fields.size();
         ++field_index) {
        lua_pushvalue(state, chunk_index + static_cast<int>(field_index + 1));
    }
    lua_pushcclosure(
        state,
        chunk_query_refill,
        "DynamicQuery.chunk_refill",
        static_cast<int>(fields.size() + 1)
    );
    lua_remove(state, chunk_index);
    lua_insert(state, chunk_index);
    return static_cast<int>(fields.size() + 1);
}

} // namespace ets::detail
