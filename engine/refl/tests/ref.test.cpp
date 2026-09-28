#include "refl/ref.hpp"

#include "refl/ref_utils.hpp"
#include "refl/registry.hpp"
#include "refl/type.hpp"
#include "test_types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace ets;
using namespace ets::refl_test;

namespace {

enum class RefAsEnum { One = 1, Two = 2 };

} // namespace

TEST_CASE(
    "Ref wraps object references without taking ownership",
    "[refl][ref]"
) {
    Registry::instance().register_type<TestStruct>();

    SECTION("Basic functionality") {
        TestStruct test {42, 3.14f};

        Ref ref = make_ref(test);
        REQUIRE(ref);
        REQUIRE(ref.type_id() == type<TestStruct>().id());
        REQUIRE_FALSE(ref.is_const());
        REQUIRE(ref.ptr() == &test);
        REQUIRE(ref.try_get<TestStruct>() == &test);
        REQUIRE(ref.try_get<float>() == nullptr);

        TestStruct& ref_test = ref.get<TestStruct>();
        REQUIRE(ref_test.a == 42);
        REQUIRE(ref_test.b == 3.14f);

        ref_test.a = 100;
        REQUIRE(test.a == 100);
    }

    SECTION("Const reference") {
        const TestStruct test {42, 3.14f};
        Ref ref = make_ref(test);

        REQUIRE(ref);
        REQUIRE(ref.type_id() == type<TestStruct>().id());
        REQUIRE(ref.is_const());
        REQUIRE(ref.try_get<TestStruct>() == nullptr);
        REQUIRE(ref.try_get_const<TestStruct>() == &test);
        REQUIRE(
            static_cast<const void*>(ref.ptr()) ==
            static_cast<const void*>(&test)
        );

        const TestStruct& ref_test = ref.get_const<TestStruct>();
        REQUIRE(ref_test.a == 42);
        REQUIRE(ref_test.b == 3.14f);
    }

    SECTION("Pointer") {
        TestStruct* test_ptr = new TestStruct {42, 3.14f};
        Ref ref = make_ref(test_ptr);

        REQUIRE(ref);
        REQUIRE(ref.type_id() == type<TestStruct>().id());
        REQUIRE_FALSE(ref.is_const());
        REQUIRE(ref.ptr() == test_ptr);
        REQUIRE(ref.get<TestStruct>().a == 42);

        delete test_ptr;
    }

    SECTION("Const pointer") {
        const TestStruct* test_ptr = new TestStruct {42, 3.14f};
        Ref ref = make_ref(test_ptr);

        REQUIRE(ref);
        REQUIRE(ref.type_id() == type<TestStruct>().id());
        REQUIRE(ref.is_const());
        REQUIRE(ref.try_get<TestStruct>() == nullptr);
        REQUIRE(ref.try_get_const<TestStruct>() == test_ptr);
        REQUIRE(
            static_cast<const void*>(ref.ptr()) ==
            static_cast<const void*>(test_ptr)
        );

        delete test_ptr;
    }

    SECTION("Null reference") {
        Ref ref;
        REQUIRE_FALSE(ref);

        ref = Ref(nullptr);
        REQUIRE_FALSE(ref);
    }

    SECTION("Comparison operators") {
        TestStruct test1 {1, 1.0f};
        TestStruct test2 {2, 2.0f};

        Ref ref1 = make_ref(test1);
        Ref ref2 = make_ref(test1);
        Ref ref3 = make_ref(test2);

        REQUIRE(ref1 == ref2);
        REQUIRE(ref1 != ref3);
        REQUIRE_FALSE(ref1 == ref3);
        REQUIRE_FALSE(ref1 != ref2);
    }
}

TEST_CASE("Ref converts arithmetic values through to_number", "[refl][ref]") {
    int integer = 42;
    float real = 2.5f;
    bool flag = true;

    REQUIRE(make_ref(integer).to_number<float>() == 42.0f);
    REQUIRE(make_ref(real).to_number<int>() == 2);
    REQUIRE(make_ref(flag).to_number<int>() == 1);
    char character = 'A';
    char8_t byte = u8'A';
    char16_t word = u'A';
    char32_t codepoint = U'\U0001F600';
    wchar_t wide = L'A';
    REQUIRE(make_ref(character).to_number<int>() == 65);
    REQUIRE(make_ref(byte).to_number<int>() == 65);
    REQUIRE(make_ref(word).to_number<int>() == 65);
    REQUIRE(make_ref(codepoint).to_number<int>() == 128512);
    REQUIRE(make_ref(wide).to_number<int>() == 65);
}

TEST_CASE("Ref exposes typed values through direct accessors", "[refl][ref]") {
    int value = 42;
    Ref ref = make_ref(value);

    static_assert(std::is_same_v<decltype(ref.get<int>()), int&>);
    static_assert(std::is_same_v<decltype(ref.get_const<int>()), const int&>);

    REQUIRE(ref.try_get<int>() == &value);
    REQUIRE(ref.try_get_const<int>() == &value);
    REQUIRE(ref.get<int>() == 42);
    ref.get<int>() = 7;
    REQUIRE(value == 7);
    REQUIRE(ref.get_const<int>() == 7);
    REQUIRE(ref.get_rref<int>() == 7);

    const int const_value = 9;
    Ref const_ref = make_ref(const_value);

    REQUIRE(const_ref.try_get<int>() == nullptr);
    REQUIRE(const_ref.try_get_const<int>() == &const_value);
    REQUIRE(const_ref.get_const<int>() == 9);
}

TEST_CASE("Ref exposes enum objects through direct accessors", "[refl][ref]") {
    RefAsEnum actual = RefAsEnum::One;
    Ref enum_ref = make_ref(actual);

    REQUIRE(enum_ref.type_id() == type_id<RefAsEnum>());
    REQUIRE(enum_ref.get<RefAsEnum>() == RefAsEnum::One);
    enum_ref.get<RefAsEnum>() = RefAsEnum::Two;
    REQUIRE(actual == RefAsEnum::Two);
}
