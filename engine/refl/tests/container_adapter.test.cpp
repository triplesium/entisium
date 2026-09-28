#include "refl/container_adapter.hpp"

#include "base/optional.hpp"
#include "base/result.hpp"
#include "refl/cls.hpp"
#include "refl/dynamic_array.hpp"
#include "refl/dynamic_map.hpp"
#include "refl/registry.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace ets;

namespace {

struct PropertyContainerFixture {
    std::vector<std::string> names;
    std::array<int, 2> weights {};
    Optional<float> ratio;
    std::unordered_map<std::string, int> scores;
    std::map<int, std::string> labels;
    std::set<std::string> tags;
    std::unordered_set<int> flags;
    std::pair<int, std::string> pair;
    std::tuple<int, float, std::string> tuple;
};

struct UniquePtrIntLess {
    bool operator()(
        const std::unique_ptr<int>& lhs,
        const std::unique_ptr<int>& rhs
    ) const {
        return *lhs < *rhs;
    }
};

struct ConstructibleButNotAssignable {
    int value {0};

    ConstructibleButNotAssignable() = default;
    explicit ConstructibleButNotAssignable(int value) : value(value) {}
    ConstructibleButNotAssignable(const ConstructibleButNotAssignable&) =
        default;
    ConstructibleButNotAssignable(ConstructibleButNotAssignable&&) noexcept =
        default;
    ConstructibleButNotAssignable&
    operator=(const ConstructibleButNotAssignable&) = delete;
    ConstructibleButNotAssignable&
    operator=(ConstructibleButNotAssignable&&) = delete;
};

bool contains_exactly(
    const std::vector<TypeId>& values,
    std::initializer_list<TypeId> expected
) {
    return values == std::vector<TypeId>(expected);
}

TypeId generic_id(std::string_view name) {
    return TypeId(name);
}

template<class T>
ContainerAdapter& require_registered_adapter_from_property(
    std::string_view generic_name,
    std::initializer_list<TypeId> argument_type_ids
) {
    Registry& registry = Registry::instance();

    auto container_result = registry.try_get_container_adapter<T>();
    REQUIRE(container_result);

    auto generic_result = registry.try_get_generic_type<T>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->generic_type_id == generic_id(generic_name));
    REQUIRE(
        contains_exactly(generic_result->argument_type_ids, argument_type_ids)
    );

    return *container_result;
}

} // namespace

TEST_CASE(
    "Container adapters expose consistent semantic kinds",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    auto require_kind = [&]<class T>(ContainerKind expected) {
        registry.register_type<T>();
        auto result = registry.try_get_container_adapter<T>();
        REQUIRE(result);

        ContainerAdapter& container = *result;
        REQUIRE(container.kind() == expected);
        const bool is_associative =
            expected == ContainerKind::Map || expected == ContainerKind::Set;
        REQUIRE((container.associative() != nullptr) == is_associative);
        REQUIRE((container.indexed() != nullptr) == !is_associative);

        if (auto* associative = container.associative()) {
            REQUIRE(
                associative->has_mapped_value() ==
                (expected == ContainerKind::Map)
            );
        }
    };

    require_kind.operator()<std::vector<int>>(ContainerKind::Sequence);
    require_kind.operator()<std::array<int, 2>>(ContainerKind::Sequence);
    require_kind.operator()<Optional<int>>(ContainerKind::Optional);
    require_kind.operator()<std::pair<int, float>>(ContainerKind::Product);
    require_kind.operator()<std::tuple<int, float>>(ContainerKind::Product);
    require_kind.operator()<std::map<int, float>>(ContainerKind::Map);
    require_kind.operator()<std::unordered_map<int, float>>(ContainerKind::Map);
    require_kind.operator()<std::set<int>>(ContainerKind::Set);
    require_kind.operator()<std::unordered_set<int>>(ContainerKind::Set);
}

TEST_CASE(
    "Indexed container methods invoke adapters through reflection",
    "[refl][container][method]"
) {
    Registry& registry = Registry::instance();
    using TestVector = std::vector<int>;
    registry.register_type<TestVector>();

    auto cls_result = registry.try_get_cls<TestVector>();
    REQUIRE(cls_result);
    auto& cls = *cls_result;
    REQUIRE(cls.has_method("size"));
    REQUIRE(cls.has_method("at"));
    REQUIRE(cls.has_method("assign"));
    REQUIRE(cls.has_method("append"));
    REQUIRE(cls.has_method("insert"));
    REQUIRE(cls.has_method("erase"));
    REQUIRE(cls.has_method("clear"));

    TestVector values;
    int value = 7;
    std::vector<Ref> append_args {Ref(values), Ref(value)};
    auto append_method = cls.get_method_for_args(
        "append",
        append_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(append_method);
    auto appended = append_method->invoke_variadic(append_args);
    REQUIRE(appended);
    REQUIRE(appended->is_status());
    REQUIRE(values == TestVector {7});

    std::vector<Ref> size_args {Ref(values)};
    auto size_method = cls.get_method_for_args(
        "size",
        size_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(size_method);
    auto size = size_method->invoke_variadic(size_args);
    REQUIRE(size);
    REQUIRE(size->is_value());
    REQUIRE(size->value().get<std::size_t>() == 1);

    int index = 0;
    std::vector<Ref> at_args {Ref(values), Ref(index)};
    auto at_method = cls.get_method_for_args(
        "at",
        at_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(at_method);
    auto element = at_method->invoke_variadic(at_args);
    REQUIRE(element);
    REQUIRE(element->is_ref());
    REQUIRE_FALSE(element->ref().is_const());
    element->ref().get<int>() = 9;
    REQUIRE(values[0] == 9);

    int replacement = 11;
    std::vector<Ref> assign_args {
        Ref(values),
        Ref(index),
        Ref(replacement),
    };
    auto assign_method = cls.get_method_for_args(
        "assign",
        assign_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(assign_method);
    REQUIRE(assign_method->invoke_variadic(assign_args));
    REQUIRE(values[0] == 11);

    const TestVector& const_values = values;
    std::vector<Ref> const_at_args {Ref(const_values), Ref(index)};
    auto const_at_method = cls.get_method_for_args(
        "at",
        const_at_args,
        MethodConstFilter::ConstOnly
    );
    REQUIRE(const_at_method);
    auto const_element = const_at_method->invoke_variadic(const_at_args);
    REQUIRE(const_element);
    REQUIRE(const_element->ref().is_const());

    std::vector<Ref> const_append_args {Ref(const_values), Ref(value)};
    auto const_append = cls.get_method_for_args(
        "append",
        const_append_args,
        MethodConstFilter::ConstOnly
    );
    REQUIRE_FALSE(const_append);

    using TestArray = std::array<int, 2>;
    registry.register_type<TestArray>();
    auto& array_cls = registry.get_cls<TestArray>();
    REQUIRE(array_cls.has_method("size"));
    REQUIRE(array_cls.has_method("at"));
    REQUIRE(array_cls.has_method("assign"));
    REQUIRE_FALSE(array_cls.has_method("append"));
    REQUIRE_FALSE(array_cls.has_method("insert"));
    REQUIRE_FALSE(array_cls.has_method("erase"));
    REQUIRE_FALSE(array_cls.has_method("clear"));
}

TEST_CASE(
    "Dynamic container methods validate instance schemas during invocation",
    "[refl][container][method][dynamic-array]"
) {
    Registry& registry = Registry::instance();
    registry.register_type<int>();
    registry.register_type<float>();

    auto array_result = DynamicArray::create(type_id<int>());
    REQUIRE(array_result);
    auto& array = *array_result;
    auto& cls = registry.get_cls<DynamicArray>();

    int value = 4;
    std::vector<Ref> append_args {Ref(array), Ref(value)};
    auto append_method = cls.get_method_for_args(
        "append",
        append_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(append_method);
    REQUIRE(append_method->invoke_variadic(append_args));
    REQUIRE(array.at(0)->get_const<int>() == 4);

    float wrong = 1.5F;
    std::vector<Ref> wrong_args {Ref(array), Ref(wrong)};
    auto same_method = cls.get_method_for_args(
        "append",
        wrong_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(same_method);
    auto wrong_result = same_method->invoke_variadic(wrong_args);
    REQUIRE_FALSE(wrong_result);
    REQUIRE(wrong_result.error().kind == InvokeFailure::Kind::ReturnedError);
    REQUIRE(
        wrong_result.error().error.get<ContainerError>().kind ==
        ContainerError::Kind::InvalidElement
    );

    auto map_result = DynamicMap::create(type_id<int>(), type_id<float>());
    REQUIRE(map_result);
    auto& map = *map_result;
    auto& map_cls = registry.get_cls<DynamicMap>();

    int key = 2;
    float mapped = 2.5F;
    std::vector<Ref> insert_args {Ref(map), Ref(key), Ref(mapped)};
    auto insert_method = map_cls.get_method_for_args(
        "insert",
        insert_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(insert_method);
    REQUIRE(insert_method->invoke_variadic(insert_args));
    REQUIRE(map.find(Ref(key))->get_const<float>() == 2.5F);

    std::vector<Ref> wrong_insert_args {Ref(map), Ref(key), Ref(value)};
    auto same_insert_method = map_cls.get_method_for_args(
        "insert",
        wrong_insert_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(same_insert_method);
    auto wrong_insert = same_insert_method->invoke_variadic(wrong_insert_args);
    REQUIRE_FALSE(wrong_insert);
    REQUIRE(wrong_insert.error().kind == InvokeFailure::Kind::ReturnedError);
    REQUIRE(
        wrong_insert.error().error.get<ContainerError>().kind ==
        ContainerError::Kind::InvalidElement
    );
}

TEST_CASE(
    "Associative container methods invoke map and set adapters",
    "[refl][container][method][associative]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::map<int, std::string>;
    registry.register_type<TestMap>();
    auto& map_cls = registry.get_cls<TestMap>();
    REQUIRE(map_cls.has_method("size"));
    REQUIRE(map_cls.has_method("find"));
    REQUIRE(map_cls.has_method("contains"));
    REQUIRE(map_cls.has_method("insert"));
    REQUIRE(map_cls.has_method("erase"));
    REQUIRE(map_cls.has_method("clear"));
    REQUIRE_FALSE(map_cls.has_method("at"));
    REQUIRE_FALSE(map_cls.has_method("append"));

    TestMap map;
    int key = 3;
    std::string value = "three";
    std::vector<Ref> insert_args {Ref(map), Ref(key), Ref(value)};
    auto insert_method = map_cls.get_method_for_args(
        "insert",
        insert_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(insert_method);
    REQUIRE(insert_method->invoke_variadic(insert_args));
    REQUIRE(map.at(3) == "three");

    std::vector<Ref> find_args {Ref(map), Ref(key)};
    auto find_method = map_cls.get_method_for_args(
        "find",
        find_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(find_method);
    auto found = find_method->invoke_variadic(find_args);
    REQUIRE(found);
    REQUIRE(found->is_ref());
    REQUIRE(found->ref().get_const<std::string>() == "three");

    auto contains_method = map_cls.get_method_for_args(
        "contains",
        find_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(contains_method);
    auto contains = contains_method->invoke_variadic(find_args);
    REQUIRE(contains);
    REQUIRE(contains->value().get<bool>());

    using TestSet = std::set<int>;
    registry.register_type<TestSet>();
    auto& set_cls = registry.get_cls<TestSet>();
    TestSet set;
    std::vector<Ref> set_insert_args {Ref(set), Ref(key)};
    auto set_insert = set_cls.get_method_for_args(
        "insert",
        set_insert_args,
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(set_insert);
    REQUIRE(set_insert->invoke_variadic(set_insert_args));
    REQUIRE(set.contains(3));
}

TEST_CASE(
    "Container adapters unify static and dynamic sequences through Ref",
    "[refl][container][dynamic-array]"
) {
    Registry& registry = Registry::instance();
    registry.register_type<int>();
    registry.register_type<float>();
    registry.register_type<std::vector<int>>();

    auto int_array_result = DynamicArray::create(type_id<int>());
    auto float_array_result = DynamicArray::create(type_id<float>());
    REQUIRE(int_array_result);
    REQUIRE(float_array_result);
    auto& int_array = *int_array_result;
    auto& float_array = *float_array_result;

    auto dynamic_result =
        registry.try_get_container_adapter(type_id<DynamicArray>());
    REQUIRE(dynamic_result);
    auto* dynamic_adapter = dynamic_result->indexed();
    REQUIRE(dynamic_adapter != nullptr);
    auto& dynamic = *dynamic_adapter;
    REQUIRE(dynamic.kind() == ContainerKind::Sequence);
    REQUIRE_FALSE(dynamic.element_type());

    auto int_schema = dynamic.element_type(Ref(int_array));
    auto float_schema = dynamic.element_type(Ref(float_array));
    REQUIRE(int_schema);
    REQUIRE(float_schema);
    REQUIRE(*int_schema == type_id<int>());
    REQUIRE(*float_schema == type_id<float>());
    REQUIRE(*dynamic.element_type(Ref(int_array), 99) == type_id<int>());

    auto static_result =
        registry.try_get_container_adapter(type_id<std::vector<int>>());
    REQUIRE(static_result);
    auto* static_indexed = static_result->indexed();
    REQUIRE(static_indexed != nullptr);
    auto& static_container = *static_indexed;
    std::vector<int> static_values;
    REQUIRE(
        *static_container.element_type(Ref(static_values)) == type_id<int>()
    );

    auto append_and_size =
        [&](Ref container, Ref value) -> Result<std::size_t, ContainerError> {
        auto* adapter =
            registry.get_container_adapter(container.type_id()).indexed();
        REQUIRE(adapter != nullptr);
        auto status = adapter->append(container, value);
        if (!status) {
            return failure(std::move(status.error()));
        }
        return adapter->size(container);
    };

    int value = 7;
    auto static_size = append_and_size(Ref(static_values), Ref(value));
    auto dynamic_size = append_and_size(Ref(int_array), Ref(value));
    REQUIRE(static_size);
    REQUIRE(dynamic_size);
    REQUIRE(*static_size == 1);
    REQUIRE(*dynamic_size == 1);
    REQUIRE(static_values == std::vector<int> {7});
    REQUIRE(int_array.at(0)->get_const<int>() == 7);

    auto insert_read_erase = [&](Ref container,
                                 Ref inserted) -> Result<int, ContainerError> {
        auto& adapter = registry.get_container_adapter(container.type_id());
        auto* indexed = adapter.indexed();
        REQUIRE(indexed != nullptr);
        auto insert_status = indexed->insert(container, 0, inserted);
        if (!insert_status) {
            return failure(std::move(insert_status.error()));
        }
        auto element = indexed->at(container, 0);
        if (!element) {
            return failure(std::move(element.error()));
        }
        auto result = element->get_const<int>();
        auto erase_status = indexed->erase(container, 0);
        if (!erase_status) {
            return failure(std::move(erase_status.error()));
        }
        return result;
    };

    int inserted = 5;
    auto static_inserted = insert_read_erase(Ref(static_values), Ref(inserted));
    auto dynamic_inserted = insert_read_erase(Ref(int_array), Ref(inserted));
    REQUIRE(static_inserted);
    REQUIRE(dynamic_inserted);
    REQUIRE(*static_inserted == 5);
    REQUIRE(*dynamic_inserted == 5);
    REQUIRE(static_values == std::vector<int> {7});
    REQUIRE(int_array.at(0)->get_const<int>() == 7);

    value = 11;
    REQUIRE(dynamic.assign(Ref(int_array), 0, Ref(value)));
    REQUIRE(int_array.at(0)->get_const<int>() == 11);

    float wrong = 1.5F;
    auto wrong_type = dynamic.append(Ref(int_array), Ref(wrong));
    REQUIRE_FALSE(wrong_type);
    REQUIRE(wrong_type.error().kind == ContainerError::Kind::InvalidElement);

    auto out_of_range = dynamic.assign(Ref(int_array), 4, Ref(value));
    REQUIRE_FALSE(out_of_range);
    REQUIRE(out_of_range.error().kind == ContainerError::Kind::OutOfRange);
    auto missing_element = dynamic.at(Ref(int_array), 4);
    REQUIRE_FALSE(missing_element);
    REQUIRE(missing_element.error().kind == ContainerError::Kind::OutOfRange);

    const DynamicArray& const_array = int_array;
    auto const_element = dynamic.at(Ref(const_array), 0);
    REQUIRE(const_element);
    REQUIRE(const_element->is_const());
    REQUIRE(const_element->get_const<int>() == 11);
    auto const_append = dynamic.append(Ref(const_array), Ref(value));
    REQUIRE_FALSE(const_append);
    REQUIRE(
        const_append.error().kind == ContainerError::Kind::InvalidContainer
    );

    bool visited_const = false;
    REQUIRE(dynamic.for_each(
        Ref(const_array),
        [&](Ref element, std::size_t index) -> Status<ContainerError> {
            REQUIRE(index == 0);
            REQUIRE(element.get_const<int>() == 11);
            visited_const = element.is_const();
            return {};
        }
    ));
    REQUIRE(visited_const);

    int not_a_container = 0;
    auto invalid_schema = dynamic.element_type(Ref(not_a_container));
    REQUIRE_FALSE(invalid_schema);
    REQUIRE(
        invalid_schema.error().kind == ContainerError::Kind::InvalidContainer
    );
}

TEST_CASE(
    "Dynamic map adapters expose instance schemas and borrowed entries",
    "[refl][container][dynamic-map]"
) {
    Registry& registry = Registry::instance();
    registry.register_type<int>();
    registry.register_type<float>();
    registry.register_type<std::string>();
    registry.register_type<std::map<int, std::string>>();

    auto map_result =
        DynamicMap::create(type_id<int>(), type_id<std::string>());
    auto other_result =
        DynamicMap::create(type_id<std::string>(), type_id<float>());
    REQUIRE(map_result);
    REQUIRE(other_result);
    auto& map = *map_result;
    auto& other = *other_result;

    auto adapter_result =
        registry.try_get_container_adapter(type_id<DynamicMap>());
    REQUIRE(adapter_result);
    auto* adapter = adapter_result->associative();
    REQUIRE(adapter != nullptr);
    REQUIRE(adapter_result->indexed() == nullptr);
    REQUIRE(adapter->kind() == ContainerKind::Map);
    REQUIRE_FALSE(adapter->key_type());
    REQUIRE_FALSE(adapter->mapped_type());
    REQUIRE(*adapter->key_type(Ref(map)) == type_id<int>());
    REQUIRE(*adapter->mapped_type(Ref(map)) == type_id<std::string>());
    REQUIRE(*adapter->key_type(Ref(other)) == type_id<std::string>());
    REQUIRE(*adapter->mapped_type(Ref(other)) == type_id<float>());

    int key = 3;
    std::string value = "three";
    std::map<int, std::string> static_map;

    auto insert_and_find = [&](Ref container, Ref map_key, Ref mapped_value)
        -> Result<Ref, ContainerError> {
        auto& container_adapter =
            registry.get_container_adapter(container.type_id());
        auto* associative = container_adapter.associative();
        if (!associative) {
            return failure(
                ContainerError::make(
                    ContainerError::Kind::InvalidContainer,
                    container.type_id(),
                    "Container is not associative"
                )
            );
        }
        auto insert_status = associative->insert(
            container,
            AssociativeElementRef {.key = map_key, .value = mapped_value}
        );
        if (!insert_status) {
            return failure(std::move(insert_status.error()));
        }
        return associative->find(container, map_key);
    };

    auto dynamic_found = insert_and_find(Ref(map), Ref(key), Ref(value));
    auto static_found = insert_and_find(Ref(static_map), Ref(key), Ref(value));
    REQUIRE(dynamic_found);
    REQUIRE(static_found);
    REQUIRE_FALSE(dynamic_found->is_const());
    REQUIRE_FALSE(static_found->is_const());
    REQUIRE(dynamic_found->get_const<std::string>() == "three");
    REQUIRE(static_found->get_const<std::string>() == "three");
    REQUIRE(*adapter->contains(Ref(map), Ref(key)));

    value = "changed";
    REQUIRE(map.find(Ref(key))->get_const<std::string>() == "three");
    REQUIRE(static_map.at(3) == "three");

    bool visited = false;
    REQUIRE(adapter->for_each_entry(
        Ref(map),
        [&](AssociativeElementRef entry,
            std::size_t index) -> Status<ContainerError> {
            REQUIRE(index == 0);
            REQUIRE(entry.key.is_const());
            REQUIRE_FALSE(entry.value.is_const());
            REQUIRE(entry.key.get_const<int>() == 3);
            REQUIRE(entry.value.get_const<std::string>() == "three");
            visited = true;
            return {};
        }
    ));
    REQUIRE(visited);

    float wrong_value = 4.0F;
    auto wrong_type = adapter->insert(
        Ref(map),
        AssociativeElementRef {.key = Ref(key), .value = Ref(wrong_value)}
    );
    REQUIRE_FALSE(wrong_type);
    REQUIRE(wrong_type.error().kind == ContainerError::Kind::InvalidElement);

    const DynamicMap& const_map = map;
    auto const_found = adapter->find(Ref(const_map), Ref(key));
    REQUIRE(const_found);
    REQUIRE(const_found->is_const());
    auto const_insert = adapter->insert(
        Ref(const_map),
        AssociativeElementRef {.key = Ref(key), .value = Ref(value)}
    );
    REQUIRE_FALSE(const_insert);
    REQUIRE(
        const_insert.error().kind == ContainerError::Kind::InvalidContainer
    );

    REQUIRE(adapter->for_each_entry(
        Ref(const_map),
        [](AssociativeElementRef entry, std::size_t) -> Status<ContainerError> {
            REQUIRE(entry.key.is_const());
            REQUIRE(entry.value.is_const());
            return {};
        }
    ));

    auto visitor_failure = adapter->for_each_entry(
        Ref(map),
        [](AssociativeElementRef, std::size_t) -> Status<ContainerError> {
            return failure(
                ContainerError::make(
                    ContainerError::Kind::UnsupportedOperation,
                    type_id<DynamicMap>(),
                    "stop"
                )
            );
        }
    );
    REQUIRE_FALSE(visitor_failure);
    REQUIRE(
        visitor_failure.error().kind ==
        ContainerError::Kind::UnsupportedOperation
    );
    REQUIRE(visitor_failure.error().message == "stop");

    int missing_key = 99;
    auto missing = adapter->find(Ref(map), Ref(missing_key));
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().kind == ContainerError::Kind::NotFound);
    auto missing_contains = adapter->contains(Ref(map), Ref(missing_key));
    REQUIRE(missing_contains);
    REQUIRE_FALSE(*missing_contains);
    auto missing_erase = adapter->erase(Ref(map), Ref(missing_key));
    REQUIRE_FALSE(missing_erase);
    REQUIRE(missing_erase.error().kind == ContainerError::Kind::NotFound);
    float wrong_key = 99.0F;
    auto wrong_contains = adapter->contains(Ref(map), Ref(wrong_key));
    REQUIRE_FALSE(wrong_contains);
    REQUIRE(
        wrong_contains.error().kind == ContainerError::Kind::InvalidElement
    );

    auto* static_adapter =
        registry.get_container_adapter(Ref(static_map).type_id()).associative();
    REQUIRE(static_adapter != nullptr);
    REQUIRE(static_adapter->erase(Ref(static_map), Ref(key)));
    REQUIRE_FALSE(*static_adapter->contains(Ref(static_map), Ref(key)));
    auto static_missing = static_adapter->find(Ref(static_map), Ref(key));
    REQUIRE_FALSE(static_missing);
    REQUIRE(static_missing.error().kind == ContainerError::Kind::NotFound);
    REQUIRE(adapter->erase(Ref(map), Ref(key)));
    REQUIRE_FALSE(*adapter->contains(Ref(map), Ref(key)));

    REQUIRE(adapter->clear(Ref(map)));
    REQUIRE(map.empty());
    REQUIRE(map.key_type() == type_id<int>());
    REQUIRE(map.mapped_type() == type_id<std::string>());
}

TEST_CASE(
    "Fixed and optional adapters expose borrowed indexed access",
    "[refl][container][at]"
) {
    Registry& registry = Registry::instance();

    using TestArray = std::array<int, 2>;
    registry.register_type<TestArray>();
    auto* array_adapter = registry.get_container_adapter<TestArray>().indexed();
    REQUIRE(array_adapter != nullptr);
    TestArray array {1, 2};

    auto array_element = array_adapter->at(Ref(array), 1);
    REQUIRE(array_element);
    REQUIRE_FALSE(array_element->is_const());
    array_element->get<int>() = 4;
    REQUIRE(array[1] == 4);

    const TestArray& const_array = array;
    auto const_array_element = array_adapter->at(Ref(const_array), 0);
    REQUIRE(const_array_element);
    REQUIRE(const_array_element->is_const());
    REQUIRE(const_array_element->get_const<int>() == 1);

    int value = 3;
    auto fixed_insert = array_adapter->insert(Ref(array), 0, Ref(value));
    auto fixed_erase = array_adapter->erase(Ref(array), 0);
    REQUIRE_FALSE(fixed_insert);
    REQUIRE_FALSE(fixed_erase);
    REQUIRE(
        fixed_insert.error().kind == ContainerError::Kind::UnsupportedOperation
    );
    REQUIRE(
        fixed_erase.error().kind == ContainerError::Kind::UnsupportedOperation
    );

    using TestProduct = std::pair<int, std::string>;
    registry.register_type<TestProduct>();
    auto* product_adapter =
        registry.get_container_adapter<TestProduct>().indexed();
    REQUIRE(product_adapter != nullptr);
    TestProduct product {5, "five"};
    auto product_value = product_adapter->at(Ref(product), 1);
    REQUIRE(product_value);
    REQUIRE(product_value->type_id() == type_id<std::string>());
    product_value->get<std::string>() = "changed";
    REQUIRE(product.second == "changed");

    auto product_missing = product_adapter->at(Ref(product), 2);
    REQUIRE_FALSE(product_missing);
    REQUIRE(product_missing.error().kind == ContainerError::Kind::OutOfRange);
    REQUIRE_FALSE(product_adapter->insert(Ref(product), 0, Ref(value)));
    REQUIRE_FALSE(product_adapter->erase(Ref(product), 0));

    using TestOptional = Optional<std::string>;
    registry.register_type<TestOptional>();
    auto* optional_adapter =
        registry.get_container_adapter<TestOptional>().indexed();
    REQUIRE(optional_adapter != nullptr);
    TestOptional optional {std::string {"value"}};
    auto optional_value = optional_adapter->at(Ref(optional), 0);
    REQUIRE(optional_value);
    REQUIRE_FALSE(optional_value->is_const());
    REQUIRE(optional_value->get_const<std::string>() == "value");

    REQUIRE(optional_adapter->erase(Ref(optional), 0));
    REQUIRE_FALSE(optional);
    auto empty_value = optional_adapter->at(Ref(optional), 0);
    REQUIRE_FALSE(empty_value);
    REQUIRE(empty_value.error().kind == ContainerError::Kind::OutOfRange);

    std::string replacement = "replacement";
    REQUIRE(optional_adapter->insert(Ref(optional), 0, Ref(replacement)));
    REQUIRE(optional);
    REQUIRE(*optional == "replacement");
}

TEST_CASE(
    "Set adapters find contain and erase keys through Ref",
    "[refl][container][set]"
) {
    Registry& registry = Registry::instance();

    using TestSet = std::set<int>;
    registry.register_type<TestSet>();
    auto* adapter = registry.get_container_adapter<TestSet>().associative();
    REQUIRE(adapter != nullptr);

    TestSet values {2, 4};
    int key = 2;
    auto found = adapter->find(Ref(values), Ref(key));
    REQUIRE(found);
    REQUIRE(found->is_const());
    REQUIRE(found->get_const<int>() == 2);
    REQUIRE(*adapter->contains(Ref(values), Ref(key)));

    int missing_key = 3;
    auto missing = adapter->find(Ref(values), Ref(missing_key));
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().kind == ContainerError::Kind::NotFound);
    REQUIRE_FALSE(*adapter->contains(Ref(values), Ref(missing_key)));

    REQUIRE(adapter->erase(Ref(values), Ref(key)));
    REQUIRE_FALSE(values.contains(2));
    auto erased_again = adapter->erase(Ref(values), Ref(key));
    REQUIRE_FALSE(erased_again);
    REQUIRE(erased_again.error().kind == ContainerError::Kind::NotFound);

    const TestSet& const_values = values;
    auto const_erase = adapter->erase(Ref(const_values), Ref(missing_key));
    REQUIRE_FALSE(const_erase);
    REQUIRE(const_erase.error().kind == ContainerError::Kind::InvalidContainer);
}

TEST_CASE(
    "Registry automatically registers vector container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    registry.register_type<std::vector<int>>();
    auto container_result =
        registry.try_get_container_adapter<std::vector<int>>();
    REQUIRE(container_result);

    auto* indexed = container_result->indexed();
    REQUIRE(indexed != nullptr);
    IndexedContainerAdapter& container = *indexed;
    REQUIRE(container.container_type() == type_id<std::vector<int>>());
    REQUIRE(container.element_type() == type_id<int>());
    REQUIRE(container.kind() == ContainerKind::Sequence);
    REQUIRE_FALSE(container.fixed_size());

    std::vector<int> values {1, 2};
    auto size = container.size(Ref(values));
    REQUIRE(size);
    REQUIRE(*size == 2);

    const std::vector<int>& const_values = values;
    int next = 3;
    REQUIRE(container.append(Ref(values), Ref(next)));
    REQUIRE(values == std::vector<int> {1, 2, 3});

    int replacement = 10;
    REQUIRE(container.assign(Ref(values), 0, Ref(replacement)));
    REQUIRE(values == std::vector<int> {10, 2, 3});

    std::vector<int> visited;
    REQUIRE(container.for_each(
        Ref(values),
        [&](Ref element, std::size_t index) -> Status<ContainerError> {
            REQUIRE(index == visited.size());
            visited.push_back(element.get_const<int>());
            return {};
        }
    ));
    REQUIRE(visited == std::vector<int> {10, 2, 3});

    auto out_of_range = container.assign(Ref(values), 99, Ref(next));
    REQUIRE_FALSE(out_of_range);
    REQUIRE(out_of_range.error().kind == ContainerError::Kind::OutOfRange);

    REQUIRE_FALSE(container.append(Ref(const_values), Ref(next)));

    auto generic_result = registry.try_get_generic_type<std::vector<int>>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->specialized_type_id == type_id<std::vector<int>>());
    REQUIRE(generic_result->generic_type_id == generic_id("std::vector"));
    REQUIRE(generic_result->generic_name == "std::vector");
    REQUIRE(
        contains_exactly(generic_result->argument_type_ids, {type_id<int>()})
    );
}

TEST_CASE(
    "Vector container adapters support move-only element types",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestVector = std::vector<std::unique_ptr<int>>;
    registry.register_type<TestVector>();
    auto container_result = registry.try_get_container_adapter<TestVector>();
    REQUIRE(container_result);

    auto* indexed = container_result->indexed();
    REQUIRE(indexed != nullptr);
    IndexedContainerAdapter& container = *indexed;
    TestVector values;
    auto value = std::make_unique<int>(42);
    int* raw_value = value.get();

    REQUIRE(container.append(Ref(values), Ref(value)));
    REQUIRE(value == nullptr);
    REQUIRE(values.size() == 1);
    REQUIRE(values.front().get() == raw_value);
    REQUIRE(*values.front() == 42);

    auto inserted = std::make_unique<int>(7);
    int* raw_inserted = inserted.get();
    REQUIRE(container.insert(Ref(values), 0, Ref(inserted)));
    REQUIRE(inserted == nullptr);
    REQUIRE(
        container.at(Ref(values), 0)->get_const<std::unique_ptr<int>>().get() ==
        raw_inserted
    );
    REQUIRE(container.erase(Ref(values), 0));
    REQUIRE(values.size() == 1);
    REQUIRE(values.front().get() == raw_value);

    const auto const_value = std::make_unique<int>(7);
    auto const_append = container.append(Ref(values), Ref(const_value));
    REQUIRE_FALSE(const_append);
    REQUIRE(const_append.error().kind == ContainerError::Kind::InvalidElement);
}

TEST_CASE(
    "Registry automatically registers fixed array container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestArray = std::array<float, 3>;
    registry.register_type<TestArray>();
    auto container_result = registry.try_get_container_adapter<TestArray>();
    REQUIRE(container_result);

    auto* indexed = container_result->indexed();
    REQUIRE(indexed != nullptr);
    IndexedContainerAdapter& container = *indexed;
    REQUIRE(container.kind() == ContainerKind::Sequence);
    REQUIRE(container.fixed_size());

    TestArray values {1.0F, 2.0F, 3.0F};
    auto size = container.size(Ref(values));
    REQUIRE(size);
    REQUIRE(*size == 3);

    float replacement = 4.0F;
    REQUIRE(container.assign(Ref(values), 1, Ref(replacement)));
    REQUIRE(values[1] == 4.0F);

    std::vector<float> visited;
    REQUIRE(container.for_each(
        Ref(values),
        [&](Ref element, std::size_t) -> Status<ContainerError> {
            visited.push_back(element.get_const<float>());
            return {};
        }
    ));
    REQUIRE(visited == std::vector<float> {1.0F, 4.0F, 3.0F});

    auto generic_result = registry.try_get_generic_type<TestArray>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->generic_type_id == generic_id("std::array"));
    REQUIRE(
        contains_exactly(generic_result->argument_type_ids, {type_id<float>()})
    );
    REQUIRE(generic_result->arguments.size() == 2);
    REQUIRE(
        generic_result->arguments[0] == GenericArgument::type(type_id<float>())
    );
    REQUIRE(generic_result->arguments[1] == GenericArgument::unsigned_value(3));
}

TEST_CASE(
    "Fixed array container adapters support zero-sized arrays",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestArray = std::array<int, 0>;
    registry.register_type<TestArray>();
    auto container_result = registry.try_get_container_adapter<TestArray>();
    REQUIRE(container_result);

    auto* indexed = container_result->indexed();
    REQUIRE(indexed != nullptr);
    IndexedContainerAdapter& container = *indexed;
    REQUIRE(container.kind() == ContainerKind::Sequence);
    REQUIRE(container.fixed_size());
    REQUIRE(container.element_type() == type_id<int>());

    TestArray values {};
    auto size = container.size(Ref(values));
    REQUIRE(size);
    REQUIRE(*size == 0);

    std::size_t visited = 0;
    REQUIRE(container.for_each(
        Ref(values),
        [&](Ref, std::size_t) -> Status<ContainerError> {
            ++visited;
            return {};
        }
    ));
    REQUIRE(visited == 0);

    auto generic_result = registry.try_get_generic_type<TestArray>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->arguments.size() == 2);
    REQUIRE(generic_result->arguments[1] == GenericArgument::unsigned_value(0));
}

TEST_CASE(
    "Container adapter assignment requires assignable elements",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestArray = std::array<ConstructibleButNotAssignable, 1>;
    registry.register_type<TestArray>();
    auto array_container_result =
        registry.try_get_container_adapter<TestArray>();
    REQUIRE(array_container_result);

    auto* array_indexed = array_container_result->indexed();
    REQUIRE(array_indexed != nullptr);
    IndexedContainerAdapter& array_container = *array_indexed;
    TestArray values {ConstructibleButNotAssignable {1}};

    REQUIRE(array_container.assign(Ref(values), 0, Ref(values[0])));
    REQUIRE(values[0].value == 1);

    ConstructibleButNotAssignable replacement {2};
    auto array_assignment =
        array_container.assign(Ref(values), 0, Ref(replacement));
    REQUIRE_FALSE(array_assignment);
    REQUIRE(
        array_assignment.error().kind ==
        ContainerError::Kind::UnsupportedOperation
    );
    REQUIRE(values[0].value == 1);

    using TestPair = std::pair<ConstructibleButNotAssignable, int>;
    registry.register_type<TestPair>();
    auto pair_container_result = registry.try_get_container_adapter<TestPair>();
    REQUIRE(pair_container_result);

    auto* pair_indexed = pair_container_result->indexed();
    REQUIRE(pair_indexed != nullptr);
    IndexedContainerAdapter& pair_container = *pair_indexed;
    TestPair pair {ConstructibleButNotAssignable {3}, 4};
    auto pair_assignment =
        pair_container.assign(Ref(pair), 0, Ref(replacement));
    REQUIRE_FALSE(pair_assignment);
    REQUIRE(
        pair_assignment.error().kind ==
        ContainerError::Kind::UnsupportedOperation
    );
    REQUIRE(pair.first.value == 3);
}

TEST_CASE(
    "Registry automatically registers optional container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestOptional = Optional<std::string>;
    registry.register_type<TestOptional>();
    auto container_result = registry.try_get_container_adapter<TestOptional>();
    REQUIRE(container_result);

    auto* indexed = container_result->indexed();
    REQUIRE(indexed != nullptr);
    IndexedContainerAdapter& container = *indexed;
    REQUIRE(container.kind() == ContainerKind::Optional);
    REQUIRE_FALSE(container.fixed_size());
    REQUIRE(container.element_type() == type_id<std::string>());

    TestOptional value;
    auto empty_size = container.size(Ref(value));
    REQUIRE(empty_size);
    REQUIRE(*empty_size == 0);
    std::string text {"hello"};
    REQUIRE(container.append(Ref(value), Ref(text)));
    REQUIRE(value.has_value());
    REQUIRE(*value == "hello");

    auto self_append = container.append(Ref(value), Ref(*value));
    REQUIRE_FALSE(self_append);
    REQUIRE(self_append.error().kind == ContainerError::Kind::OutOfRange);
    REQUIRE(*value == "hello");

    std::string replacement {"changed"};
    REQUIRE(container.assign(Ref(value), 0, Ref(replacement)));
    REQUIRE(*value == "changed");

    std::size_t visited = 0;
    REQUIRE(container.for_each(
        Ref(value),
        [&](Ref element, std::size_t index) -> Status<ContainerError> {
            REQUIRE(index == 0);
            REQUIRE(element.get_const<std::string>() == "changed");
            ++visited;
            return {};
        }
    ));
    REQUIRE(visited == 1);

    REQUIRE(container.clear(Ref(value)));
    REQUIRE_FALSE(value.has_value());

    std::string appended_after_clear {"again"};
    REQUIRE(container.append(Ref(value), Ref(appended_after_clear)));
    REQUIRE(*value == "again");

    auto generic_result = registry.try_get_generic_type<TestOptional>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->generic_type_id == generic_id("ets::Optional"));
    REQUIRE(contains_exactly(
        generic_result->argument_type_ids,
        {type_id<std::string>()}
    ));
}

TEST_CASE(
    "Registry automatically registers unordered_map container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::unordered_map<std::string, int>;
    registry.register_type<TestMap>();
    auto container_result = registry.try_get_container_adapter<TestMap>();
    REQUIRE(container_result);

    ContainerAdapter& container = *container_result;
    REQUIRE(container.kind() == ContainerKind::Map);
    REQUIRE(container.indexed() == nullptr);

    auto* map_container = container.associative();
    REQUIRE(map_container != nullptr);
    REQUIRE(map_container->has_mapped_value());
    REQUIRE(map_container->key_type() == type_id<std::string>());
    REQUIRE(map_container->mapped_type() == type_id<int>());

    TestMap values {{"one", 1}};

    std::string two {"two"};
    int two_value = 2;
    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(two), .value = Ref(two_value)}
    ));
    REQUIRE(values.at("two") == 2);

    two_value = 22;
    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(two), .value = Ref(two_value)}
    ));
    REQUIRE(values.at("two") == 22);

    int visited_sum = 0;
    std::size_t visited_count = 0;
    REQUIRE(map_container->for_each_entry(
        Ref(values),
        [&](AssociativeElementRef entry,
            std::size_t) -> Status<ContainerError> {
            REQUIRE(entry.key.is_const());
            visited_sum += entry.value.get_const<int>();
            ++visited_count;
            return {};
        }
    ));
    REQUIRE(visited_count == 2);
    REQUIRE(visited_sum == 23);

    auto generic_result = registry.try_get_generic_type<TestMap>();
    REQUIRE(generic_result);
    REQUIRE(
        generic_result->generic_type_id == generic_id("std::unordered_map")
    );
    REQUIRE(contains_exactly(
        generic_result->argument_type_ids,
        {type_id<std::string>(), type_id<int>()}
    ));
}

TEST_CASE(
    "Registry automatically registers ordered map container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::map<std::string, int>;
    registry.register_type<TestMap>();
    auto container_result = registry.try_get_container_adapter<TestMap>();
    REQUIRE(container_result);

    ContainerAdapter& container = *container_result;
    REQUIRE(container.kind() == ContainerKind::Map);
    REQUIRE(container.indexed() == nullptr);

    auto* map_container = container.associative();
    REQUIRE(map_container != nullptr);
    REQUIRE(map_container->has_mapped_value());
    REQUIRE(map_container->key_type() == type_id<std::string>());
    REQUIRE(map_container->mapped_type() == type_id<int>());

    TestMap values {{"one", 1}};
    std::string two {"two"};
    int two_value = 2;
    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(two), .value = Ref(two_value)}
    ));
    REQUIRE(values.at("two") == 2);

    std::vector<std::string> visited_keys;
    REQUIRE(map_container->for_each_entry(
        Ref(values),
        [&](AssociativeElementRef entry,
            std::size_t) -> Status<ContainerError> {
            visited_keys.push_back(entry.key.get_const<std::string>());
            return {};
        }
    ));
    REQUIRE(visited_keys == std::vector<std::string> {"one", "two"});

    auto generic_result = registry.try_get_generic_type<TestMap>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->generic_type_id == generic_id("std::map"));
    REQUIRE(contains_exactly(
        generic_result->argument_type_ids,
        {type_id<std::string>(), type_id<int>()}
    ));
}

TEST_CASE(
    "Registry automatically registers set container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestSet = std::set<std::string>;
    registry.register_type<TestSet>();
    auto container_result = registry.try_get_container_adapter<TestSet>();
    REQUIRE(container_result);

    ContainerAdapter& container = *container_result;
    REQUIRE(container.kind() == ContainerKind::Set);
    REQUIRE(container.indexed() == nullptr);

    auto* set_container = container.associative();
    REQUIRE(set_container != nullptr);
    REQUIRE_FALSE(set_container->has_mapped_value());
    REQUIRE(set_container->key_type() == type_id<std::string>());
    REQUIRE_FALSE(set_container->mapped_type());

    TestSet values {"one"};
    std::string two {"two"};
    REQUIRE(set_container
                ->insert(Ref(values), AssociativeElementRef {.key = Ref(two)}));
    REQUIRE(values.contains("two"));

    std::size_t visited_count = 0;
    REQUIRE(set_container->for_each_entry(
        Ref(values),
        [&](AssociativeElementRef entry,
            std::size_t) -> Status<ContainerError> {
            REQUIRE(entry.key.is_const());
            REQUIRE_FALSE(entry.value);
            ++visited_count;
            return {};
        }
    ));
    REQUIRE(visited_count == 2);

    auto generic_result = registry.try_get_generic_type<TestSet>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->generic_type_id == generic_id("std::set"));
    REQUIRE(contains_exactly(
        generic_result->argument_type_ids,
        {type_id<std::string>()}
    ));
}

TEST_CASE(
    "Registry automatically registers unordered_set container adapters",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestSet = std::unordered_set<int>;
    registry.register_type<TestSet>();
    auto container_result = registry.try_get_container_adapter<TestSet>();
    REQUIRE(container_result);

    ContainerAdapter& container = *container_result;
    REQUIRE(container.kind() == ContainerKind::Set);

    auto* set_container = container.associative();
    REQUIRE(set_container != nullptr);
    REQUIRE_FALSE(set_container->has_mapped_value());

    TestSet values;
    int value = 7;
    REQUIRE(set_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(value)}
    ));
    REQUIRE(values.contains(7));

    auto generic_result = registry.try_get_generic_type<TestSet>();
    REQUIRE(generic_result);
    REQUIRE(
        generic_result->generic_type_id == generic_id("std::unordered_set")
    );
    REQUIRE(
        contains_exactly(generic_result->argument_type_ids, {type_id<int>()})
    );
}

TEST_CASE(
    "Unordered map value_type is reflected as a const-key pair container",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::unordered_map<std::string, int>;
    using Entry = TestMap::value_type;
    registry.register_type<TestMap>();

    auto entry_container_result = registry.try_get_container_adapter<Entry>();
    REQUIRE(entry_container_result);

    auto* entry_indexed = entry_container_result->indexed();
    REQUIRE(entry_indexed != nullptr);
    IndexedContainerAdapter& entry_container = *entry_indexed;
    REQUIRE(entry_container.kind() == ContainerKind::Product);
    REQUIRE(entry_container.fixed_size());
    REQUIRE_FALSE(entry_container.element_type());
    REQUIRE(*entry_container.element_type(0) == type_id<std::string>());
    REQUIRE(*entry_container.element_type(1) == type_id<int>());

    Entry entry {"one", 1};
    std::size_t visited = 0;
    REQUIRE(entry_container.for_each(
        Ref(entry),
        [&](Ref element, std::size_t index) -> Status<ContainerError> {
            if (index == 0) {
                REQUIRE(element.is_const());
                REQUIRE(element.type_id() == type_id<std::string>());
                REQUIRE(element.get_const<std::string>() == "one");
            } else {
                REQUIRE(index == 1);
                REQUIRE_FALSE(element.is_const());
                REQUIRE(element.get<int>() == 1);
            }
            ++visited;
            return {};
        }
    ));
    REQUIRE(visited == 2);

    std::string replacement_key {"two"};
    auto key_assign =
        entry_container.assign(Ref(entry), 0, Ref(replacement_key));
    REQUIRE_FALSE(key_assign);
    REQUIRE(key_assign.error().kind == ContainerError::Kind::InvalidElement);

    int replacement_value = 10;
    REQUIRE(entry_container.assign(Ref(entry), 1, Ref(replacement_value)));
    REQUIRE(entry.second == 10);
}

TEST_CASE(
    "Registry records generic metadata for non-container templates",
    "[refl][generic]"
) {
    Registry& registry = Registry::instance();

    using TestResult = Result<int, std::string>;
    registry.register_type<TestResult>();

    auto generic_result = registry.try_get_generic_type<TestResult>();
    REQUIRE(generic_result);
    REQUIRE(generic_result->specialized_type_id == type_id<TestResult>());
    REQUIRE(generic_result->generic_type_id == generic_id("ets::Result"));
    REQUIRE(generic_result->generic_name == "ets::Result");
    REQUIRE(contains_exactly(
        generic_result->argument_type_ids,
        {type_id<int>(), type_id<std::string>()}
    ));

    auto container_result = registry.try_get_container_adapter<TestResult>();
    REQUIRE_FALSE(container_result);
    REQUIRE(
        container_result.error().kind ==
        RegistryError::Kind::ContainerAdapterNotFound
    );

    using TestPair = std::pair<int, std::string>;
    registry.register_type<TestPair>();

    auto pair_result = registry.try_get_generic_type<TestPair>();
    REQUIRE(pair_result);
    REQUIRE(pair_result->specialized_type_id == type_id<TestPair>());
    REQUIRE(pair_result->generic_type_id == generic_id("std::pair"));
    REQUIRE(pair_result->generic_name == "std::pair");
    REQUIRE(contains_exactly(
        pair_result->argument_type_ids,
        {type_id<int>(), type_id<std::string>()}
    ));
    auto pair_container_result = registry.try_get_container_adapter<TestPair>();
    REQUIRE(pair_container_result);
    auto* pair_indexed = pair_container_result->indexed();
    REQUIRE(pair_indexed != nullptr);
    IndexedContainerAdapter& pair_container = *pair_indexed;
    REQUIRE(pair_container.fixed_size());
    REQUIRE(pair_container.kind() == ContainerKind::Product);
    TestPair empty_pair {};
    auto pair_size = pair_container.size(Ref(empty_pair));
    REQUIRE(pair_size);
    REQUIRE(*pair_size == 2);
    REQUIRE_FALSE(pair_container.element_type());
    REQUIRE(*pair_container.element_type(0) == type_id<int>());
    REQUIRE(*pair_container.element_type(1) == type_id<std::string>());

    TestPair pair {7, "seven"};
    std::size_t pair_visited = 0;
    REQUIRE(pair_container.for_each(
        Ref(pair),
        [&](Ref element, std::size_t index) -> Status<ContainerError> {
            if (index == 0) {
                REQUIRE(element.get<int>() == 7);
            } else {
                REQUIRE(element.get<std::string>() == "seven");
            }
            ++pair_visited;
            return {};
        }
    ));
    REQUIRE(pair_visited == 2);
    int pair_replacement = 9;
    REQUIRE(pair_container.assign(Ref(pair), 0, Ref(pair_replacement)));
    REQUIRE(pair.first == 9);
    REQUIRE_FALSE(pair_container.assign(Ref(pair), 2, Ref(pair_replacement)));

    using TestTuple = std::tuple<int, float, std::string>;
    registry.register_type<TestTuple>();

    auto tuple_result = registry.try_get_generic_type<TestTuple>();
    REQUIRE(tuple_result);
    REQUIRE(tuple_result->specialized_type_id == type_id<TestTuple>());
    REQUIRE(tuple_result->generic_type_id == generic_id("std::tuple"));
    REQUIRE(tuple_result->generic_name == "std::tuple");
    REQUIRE(contains_exactly(
        tuple_result->argument_type_ids,
        {type_id<int>(), type_id<float>(), type_id<std::string>()}
    ));
    auto tuple_container_result =
        registry.try_get_container_adapter<TestTuple>();
    REQUIRE(tuple_container_result);
    auto* tuple_indexed = tuple_container_result->indexed();
    REQUIRE(tuple_indexed != nullptr);
    IndexedContainerAdapter& tuple_container = *tuple_indexed;
    REQUIRE(tuple_container.fixed_size());
    REQUIRE(tuple_container.kind() == ContainerKind::Product);
    REQUIRE_FALSE(tuple_container.element_type());
    REQUIRE(*tuple_container.element_type(0) == type_id<int>());
    REQUIRE(*tuple_container.element_type(1) == type_id<float>());
    REQUIRE(*tuple_container.element_type(2) == type_id<std::string>());

    TestTuple tuple {3, 1.5F, "tuple"};
    std::string tuple_replacement {"assigned"};
    REQUIRE(tuple_container.assign(Ref(tuple), 2, Ref(tuple_replacement)));
    REQUIRE(std::get<2>(tuple) == "assigned");

    using EmptyTuple = std::tuple<>;
    registry.register_type<EmptyTuple>();

    auto empty_tuple_result = registry.try_get_generic_type<EmptyTuple>();
    REQUIRE(empty_tuple_result);
    REQUIRE(empty_tuple_result->specialized_type_id == type_id<EmptyTuple>());
    REQUIRE(empty_tuple_result->generic_type_id == generic_id("std::tuple"));
    REQUIRE(empty_tuple_result->argument_type_ids.empty());

    auto empty_tuple_container_result =
        registry.try_get_container_adapter<EmptyTuple>();
    REQUIRE(empty_tuple_container_result);
    auto* empty_tuple_indexed = empty_tuple_container_result->indexed();
    REQUIRE(empty_tuple_indexed != nullptr);
    IndexedContainerAdapter& empty_tuple_container = *empty_tuple_indexed;
    REQUIRE(empty_tuple_container.fixed_size());
    REQUIRE(empty_tuple_container.kind() == ContainerKind::Product);
    REQUIRE_FALSE(empty_tuple_container.element_type());

    EmptyTuple empty_tuple;
    auto empty_tuple_size = empty_tuple_container.size(Ref(empty_tuple));
    REQUIRE(empty_tuple_size);
    REQUIRE(*empty_tuple_size == 0);
    REQUIRE_FALSE(empty_tuple_container.element_type(0));
    std::size_t empty_visited = 0;
    REQUIRE(empty_tuple_container.for_each(
        Ref(empty_tuple),
        [&](Ref, std::size_t) -> Status<ContainerError> {
            ++empty_visited;
            return {};
        }
    ));
    REQUIRE(empty_visited == 0);
}

TEST_CASE(
    "Unordered map container adapters support move-only mapped types",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::unordered_map<int, std::unique_ptr<int>>;
    registry.register_type<TestMap>();
    auto container_result = registry.try_get_container_adapter<TestMap>();
    REQUIRE(container_result);

    auto* map_container = container_result->associative();
    REQUIRE(map_container != nullptr);

    TestMap values;
    int key = 4;
    auto value = std::make_unique<int>(44);
    int* raw_value = value.get();

    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(key), .value = Ref(value)}
    ));
    REQUIRE(value == nullptr);
    REQUIRE(values.at(4).get() == raw_value);
    REQUIRE(*values.at(4) == 44);

    auto replacement = std::make_unique<int>(55);
    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(key), .value = Ref(replacement)}
    ));
    REQUIRE(replacement == nullptr);
    REQUIRE(*values.at(4) == 55);

    const auto const_value = std::make_unique<int>(66);
    auto const_insert = map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(key), .value = Ref(const_value)}
    );
    REQUIRE_FALSE(const_insert);
    REQUIRE(const_insert.error().kind == ContainerError::Kind::InvalidElement);
}

TEST_CASE(
    "Ordered map container adapters support move-only mapped types",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestMap = std::map<int, std::unique_ptr<int>>;
    registry.register_type<TestMap>();
    auto container_result = registry.try_get_container_adapter<TestMap>();
    REQUIRE(container_result);

    auto* map_container = container_result->associative();
    REQUIRE(map_container != nullptr);

    TestMap values;
    int key = 4;
    auto value = std::make_unique<int>(44);
    int* raw_value = value.get();

    REQUIRE(map_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(key), .value = Ref(value)}
    ));
    REQUIRE(value == nullptr);
    REQUIRE(values.at(4).get() == raw_value);
    REQUIRE(*values.at(4) == 44);
}

TEST_CASE(
    "Set container adapters support move-only key types",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    using TestSet = std::set<std::unique_ptr<int>, UniquePtrIntLess>;
    registry.register_type<TestSet>();
    auto container_result = registry.try_get_container_adapter<TestSet>();
    REQUIRE(container_result);

    auto* set_container = container_result->associative();
    REQUIRE(set_container != nullptr);
    REQUIRE_FALSE(set_container->has_mapped_value());

    TestSet values;
    auto key = std::make_unique<int>(44);
    int* raw_key = key.get();

    REQUIRE(set_container
                ->insert(Ref(values), AssociativeElementRef {.key = Ref(key)}));
    REQUIRE(key == nullptr);
    REQUIRE(values.size() == 1);
    REQUIRE(values.begin()->get() == raw_key);
    REQUIRE(*(*values.begin()) == 44);

    auto lookup = std::make_unique<int>(44);
    auto found = set_container->find(Ref(values), Ref(lookup));
    REQUIRE(found);
    REQUIRE(found->is_const());
    REQUIRE(found->get_const<std::unique_ptr<int>>().get() == raw_key);
    REQUIRE(*set_container->contains(Ref(values), Ref(lookup)));
    REQUIRE(set_container->erase(Ref(values), Ref(lookup)));
    REQUIRE(values.empty());

    const auto const_key = std::make_unique<int>(55);
    auto const_insert = set_container->insert(
        Ref(values),
        AssociativeElementRef {.key = Ref(const_key)}
    );
    REQUIRE_FALSE(const_insert);
    REQUIRE(const_insert.error().kind == ContainerError::Kind::InvalidElement);
}

TEST_CASE(
    "Class property registration registers container member types",
    "[refl][container]"
) {
    Registry& registry = Registry::instance();

    registry.register_cls<PropertyContainerFixture>()
        .add_property("names", &PropertyContainerFixture::names)
        .add_property("weights", &PropertyContainerFixture::weights)
        .add_property("ratio", &PropertyContainerFixture::ratio)
        .add_property("scores", &PropertyContainerFixture::scores)
        .add_property("labels", &PropertyContainerFixture::labels)
        .add_property("tags", &PropertyContainerFixture::tags)
        .add_property("flags", &PropertyContainerFixture::flags)
        .add_property("pair", &PropertyContainerFixture::pair)
        .add_property("tuple", &PropertyContainerFixture::tuple);

    auto& names =
        require_registered_adapter_from_property<std::vector<std::string>>(
            "std::vector",
            {type_id<std::string>()}
        );
    REQUIRE(names.kind() == ContainerKind::Sequence);
    REQUIRE(names.indexed() != nullptr);
    REQUIRE(names.indexed()->element_type() == type_id<std::string>());

    auto& weights =
        require_registered_adapter_from_property<std::array<int, 2>>(
            "std::array",
            {type_id<int>()}
        );
    REQUIRE(weights.kind() == ContainerKind::Sequence);
    REQUIRE(weights.indexed() != nullptr);
    REQUIRE(weights.indexed()->fixed_size());
    REQUIRE(weights.indexed()->element_type() == type_id<int>());

    auto& ratio = require_registered_adapter_from_property<Optional<float>>(
        "ets::Optional",
        {type_id<float>()}
    );
    REQUIRE(ratio.kind() == ContainerKind::Optional);
    REQUIRE(ratio.indexed() != nullptr);
    REQUIRE_FALSE(ratio.indexed()->fixed_size());
    REQUIRE(ratio.indexed()->element_type() == type_id<float>());

    auto& scores = require_registered_adapter_from_property<
        std::unordered_map<std::string, int>>(
        "std::unordered_map",
        {type_id<std::string>(), type_id<int>()}
    );
    REQUIRE(scores.kind() == ContainerKind::Map);
    REQUIRE(scores.associative()->has_mapped_value());
    REQUIRE(scores.associative()->mapped_type() == type_id<int>());

    auto& labels =
        require_registered_adapter_from_property<std::map<int, std::string>>(
            "std::map",
            {type_id<int>(), type_id<std::string>()}
        );
    REQUIRE(labels.kind() == ContainerKind::Map);
    REQUIRE(labels.associative()->has_mapped_value());
    REQUIRE(labels.associative()->mapped_type() == type_id<std::string>());

    auto& tags =
        require_registered_adapter_from_property<std::set<std::string>>(
            "std::set",
            {type_id<std::string>()}
        );
    REQUIRE(tags.kind() == ContainerKind::Set);
    REQUIRE_FALSE(tags.associative()->has_mapped_value());
    REQUIRE(tags.associative()->key_type() == type_id<std::string>());

    auto& flags =
        require_registered_adapter_from_property<std::unordered_set<int>>(
            "std::unordered_set",
            {type_id<int>()}
        );
    REQUIRE(flags.kind() == ContainerKind::Set);
    REQUIRE_FALSE(flags.associative()->has_mapped_value());
    REQUIRE(flags.associative()->key_type() == type_id<int>());

    auto& pair =
        require_registered_adapter_from_property<std::pair<int, std::string>>(
            "std::pair",
            {type_id<int>(), type_id<std::string>()}
        );
    REQUIRE(pair.kind() == ContainerKind::Product);
    REQUIRE(pair.indexed() != nullptr);
    REQUIRE(pair.indexed()->fixed_size());

    auto& tuple = require_registered_adapter_from_property<
        std::tuple<int, float, std::string>>(
        "std::tuple",
        {type_id<int>(), type_id<float>(), type_id<std::string>()}
    );
    REQUIRE(tuple.kind() == ContainerKind::Product);
    REQUIRE(tuple.indexed() != nullptr);
    REQUIRE(tuple.indexed()->fixed_size());
}

TEST_CASE(
    "Standard optional reuses reflected nullable container operations",
    "[refl][container]"
) {
    auto& registry = ets::Registry::instance();
    registry.register_type<std::optional<int>>();
    auto adapter = registry.try_get_container_adapter<std::optional<int>>();
    REQUIRE(adapter);
    REQUIRE(adapter->kind() == ets::ContainerKind::Optional);
    auto* indexed = adapter->indexed();
    REQUIRE(indexed);
    std::optional<int> value;
    int number = 42;
    REQUIRE(indexed->append(ets::Ref(value), ets::Ref(number)));
    CHECK(*value == 42);
    REQUIRE(indexed->clear(ets::Ref(value)));
    CHECK_FALSE(value);
}
