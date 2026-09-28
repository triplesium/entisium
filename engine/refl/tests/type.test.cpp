#include "refl/type.hpp"

#include "refl/generated.hpp"
#include "refl/registry.hpp"
#include "test_types.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace ets;
using namespace ets::refl_test;

namespace {

struct EqualityOnly {
    int value;

    bool operator==(const EqualityOnly&) const = default;
};

struct NoEquality {
    int value;
};

struct LateAnnotation {
    bool enabled {false};
};

struct LateAnnotatedType {};

struct TypedAnnotatedType {};

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = default;
    ThrowingMove& operator=(const ThrowingMove&) = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
};

struct ThrowingMoveAssignment {
    ThrowingMoveAssignment() = default;
    ThrowingMoveAssignment(const ThrowingMoveAssignment&) = delete;
    ThrowingMoveAssignment& operator=(const ThrowingMoveAssignment&) = delete;
    ThrowingMoveAssignment(ThrowingMoveAssignment&&) noexcept = default;
    ThrowingMoveAssignment&
    operator=(ThrowingMoveAssignment&&) noexcept(false) {
        return *this;
    }
};

} // namespace

TEST_CASE("Reflection value move contracts are explicit", "[refl][type]") {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<TestStruct>);
    STATIC_REQUIRE_FALSE(std::is_nothrow_move_constructible_v<ThrowingMove>);
    STATIC_REQUIRE(
        std::is_nothrow_move_constructible_v<ThrowingMoveAssignment>
    );

    auto& throwing_move_type =
        Registry::instance().register_type<ThrowingMove>();
    REQUIRE_FALSE(throwing_move_type.move_constructible());
    REQUIRE_FALSE(throwing_move_type.move_assignable());

    auto& type = Registry::instance().register_type<ThrowingMoveAssignment>();
    REQUIRE(type.move_constructible());
    REQUIRE_FALSE(type.move_assignable());
}

TEST_CASE("Registry records type metadata and capabilities", "[refl][type]") {
    Registry& registry = Registry::instance();

    Type& test_type = registry.register_type<TestStruct>();
    REQUIRE(test_type.id() == type_id<TestStruct>());
    REQUIRE(test_type.hash() == type_id<TestStruct>());
    REQUIRE(test_type.size() == sizeof(TestStruct));
    REQUIRE(test_type.align() == alignof(TestStruct));
    REQUIRE(test_type.default_constructible());
    REQUIRE(test_type.copy_constructible());
    REQUIRE(test_type.move_constructible());
    REQUIRE(test_type.copy_assignable());
    REQUIRE(test_type.move_assignable());
    REQUIRE(test_type.delete_func() != nullptr);
    REQUIRE(type(test_type.id()).id() == test_type.id());

    struct alignas(32) AlignedStruct {
        int value;
    };
    Type& aligned_type = registry.register_type<AlignedStruct>();
    REQUIRE(aligned_type.size() == sizeof(AlignedStruct));
    REQUIRE(aligned_type.align() == alignof(AlignedStruct));

    Type& int_type = registry.register_type<int>();
    Type& float_type = registry.register_type<float>();
    REQUIRE(int_type.is_number());
    REQUIRE(int_type.is_integral());
    REQUIRE(Registry::instance().register_type<char8_t>().is_integral());
    REQUIRE(Registry::instance().register_type<char16_t>().is_integral());
    REQUIRE(Registry::instance().register_type<char32_t>().is_integral());
    REQUIRE(Registry::instance().register_type<wchar_t>().is_integral());
    REQUIRE_FALSE(int_type.is_floating_point());
    REQUIRE(float_type.is_number());
    REQUIRE_FALSE(float_type.is_integral());
    REQUIRE(float_type.is_floating_point());

    REQUIRE(int_type.equality_comparable());
    REQUIRE(int_type.hashable());
    int lhs = 42;
    int same = 42;
    int different = 7;
    REQUIRE(int_type.equals(&lhs, &same));
    REQUIRE(*int_type.equals(&lhs, &same));
    REQUIRE_FALSE(*int_type.equals(&lhs, &different));
    REQUIRE(int_type.hash_value(&lhs));
    REQUIRE(*int_type.hash_value(&lhs) == *int_type.hash_value(&same));

    Type& equality_only_type = registry.register_type<EqualityOnly>();
    REQUIRE(equality_only_type.equality_comparable());
    REQUIRE_FALSE(equality_only_type.hashable());
    EqualityOnly equality_only {1};
    REQUIRE(equality_only_type.equals(&equality_only, &equality_only));
    REQUIRE_FALSE(equality_only_type.hash_value(&equality_only));

    Type& no_equality_type = registry.register_type<NoEquality>();
    REQUIRE_FALSE(no_equality_type.equality_comparable());
    REQUIRE_FALSE(no_equality_type.hashable());
    NoEquality no_equality {1};
    REQUIRE_FALSE(no_equality_type.equals(&no_equality, &no_equality));
    REQUIRE_FALSE(no_equality_type.hash_value(&no_equality));

    Type& vector_type = registry.register_type<std::vector<NoEquality>>();
    REQUIRE_FALSE(vector_type.equality_comparable());
    REQUIRE_FALSE(vector_type.hashable());
}

TEST_CASE(
    "Annotation schemas bind instances registered before the schema",
    "[refl][type][annotation]"
) {
    auto& registry = Registry::instance();
    registry.register_type<LateAnnotatedType>();
    auto& reflected_type =
        refl::generated::AnnotationWriter::add_field<LateAnnotatedType>(
            registry,
            "Late",
            "enabled",
            "true"
        );

    REQUIRE_FALSE(reflected_type.annotation<LateAnnotation>());

    refl::generated::AnnotationWriter::register_schema<LateAnnotation>(
        registry,
        "Late",
        {"enabled"},
        [](AnnotationView annotation) {
            LateAnnotation result;
            if (const auto enabled = annotation.value("enabled")) {
                result.enabled =
                    parse_generated_annotation_value<bool>(*enabled);
            }
            return result;
        },
        [](const LateAnnotation& annotation) {
            return std::vector<AnnotationField> {
                {"enabled",
                 format_generated_annotation_value(annotation.enabled)},
            };
        }
    );

    const auto annotation = reflected_type.annotation<LateAnnotation>();
    REQUIRE(annotation);
    CHECK(annotation->enabled);
}

TEST_CASE(
    "Generated reflection annotations preserve values",
    "[refl][type][annotation]"
) {
    Registry& registry = Registry::instance();
    register_generated_reflection();

    auto& reflected_type = registry.get_type(type_id<ReflectedTaggedType>());
    REQUIRE(reflected_type.has_structured_name());
    REQUIRE(reflected_type.namespace_path().size() == 2);
    CHECK(reflected_type.namespace_path()[0] == "ets");
    CHECK(reflected_type.namespace_path()[1] == "refl_test");
    CHECK(reflected_type.local_name() == "ReflectedTaggedType");
    const auto plugin = reflected_type.annotation("Example");
    REQUIRE(plugin);
    CHECK(plugin->name() == "Example");
    REQUIRE(plugin->value("enabled"));
    CHECK(*plugin->value("enabled") == "true");
    REQUIRE(plugin->value("name"));
    CHECK(*plugin->value("name") == "rendering");
    REQUIRE(plugin->value("phase"));
    CHECK(*plugin->value("phase") == "runtime");
    CHECK_FALSE(plugin->value("missing"));

    const auto typed_example = reflected_type.annotation<ExampleAnnotation>();
    REQUIRE(typed_example);
    CHECK(typed_example->enabled);
    CHECK(typed_example->name == "rendering");
    CHECK(typed_example->phase == "runtime");

    auto& reflected_enum = registry.get_type(type_id<ReflectedTaggedEnum>());
    REQUIRE(reflected_enum.has_structured_name());
    REQUIRE(reflected_enum.namespace_path().size() == 2);
    CHECK(reflected_enum.namespace_path()[0] == "ets");
    CHECK(reflected_enum.namespace_path()[1] == "refl_test");
    CHECK(reflected_enum.local_name() == "ReflectedTaggedEnum");
    const auto category = reflected_enum.annotation("Category");
    REQUIRE(category);
    REQUIRE(category->value("name"));
    CHECK(*category->value("name") == "example");
    const auto typed_category = reflected_enum.annotation<CategoryAnnotation>();
    REQUIRE(typed_category);
    CHECK(typed_category->name == "example");
    const auto plugin_types = registry.types_with_annotation("Example");
    CHECK(
        std::ranges::find(plugin_types, type_id<ReflectedTaggedType>()) !=
        plugin_types.end()
    );
    const auto typed_example_types =
        registry.types_with_annotation<ExampleAnnotation>();
    CHECK(
        std::ranges::find(
            typed_example_types,
            type_id<ReflectedTaggedType>()
        ) != typed_example_types.end()
    );

    auto& typed_type =
        registry.add_annotation<TypedAnnotatedType>(ExampleAnnotation {
            .enabled = true,
            .name = "typed",
            .phase = "test",
        });
    const auto typed = typed_type.annotation<ExampleAnnotation>();
    REQUIRE(typed);
    CHECK(typed->enabled);
    CHECK(typed->name == "typed");
    CHECK(typed->phase == "test");
    const auto dynamic = typed_type.annotation("Example");
    REQUIRE(dynamic);
    const auto enabled = dynamic->value("enabled");
    const auto name = dynamic->value("name");
    const auto phase = dynamic->value("phase");
    REQUIRE(enabled);
    REQUIRE(name);
    REQUIRE(phase);
    CHECK(*enabled == "true");
    CHECK(*name == "typed");
    CHECK(*phase == "test");
}

TEST_CASE(
    "Reflected numeric type identities preserve signedness and width",
    "[refl][type]"
) {
    CHECK(ets::type_id<int>() != ets::type_id<unsigned int>());
    CHECK(ets::type_id<long>() != ets::type_id<unsigned long>());
    CHECK(ets::type_id<long>() != ets::type_id<long long>());
    CHECK(ets::type_id<char>() != ets::type_id<signed char>());
    CHECK(ets::type_id<char>() != ets::type_id<unsigned char>());
}
