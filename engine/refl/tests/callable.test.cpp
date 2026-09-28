#include "base/result.hpp"
#include "refl/argument_adapter.hpp"
#include "refl/cls.hpp"
#include "refl/constructor.hpp"
#include "refl/method.hpp"
#include "refl/property.hpp"
#include "refl/ref_utils.hpp"
#include "refl/registry.hpp"
#include "refl/val.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace ets;

namespace {

enum class TestEnum { One = 1, Two = 2 };

struct CallableError {
    int code {0};
};

struct EnumPropertyFixture {
    TestEnum value {TestEnum::One};
};

struct PropertyConversionFixture {
    float scale {0.0f};
    double precise {0.0};
    int value {0};
    TestEnum mode {TestEnum::One};
    std::string_view view;
};

struct CallableFixture {
    int value {0};

    CallableFixture() = default;
    explicit CallableFixture(int value) : value(value) {}

    int add(int rhs) { return value + rhs; }
    int read() const { return value; }
    void set(int next) { value = next; }
    static int sum(int lhs, int rhs) { return lhs + rhs; }
    int enum_value(TestEnum value) const { return static_cast<int>(value); }
    int read_ptr(const int* ptr) const { return *ptr; }
    void write_ptr(int* ptr) const { *ptr = value; }
    float scale_float(float factor) const {
        return static_cast<float>(value) * factor;
    }
    double add_double(double rhs) const { return value + rhs; }
    std::size_t text_size(std::string_view text) const { return text.size(); }
};

struct ResultFixture {
    int value {5};

    Result<int, CallableError> result_value(bool succeed) const {
        if (succeed) {
            return 42;
        }
        return failure(CallableError {.code = 7});
    }

    Status<CallableError> status_value(bool succeed) const {
        if (succeed) {
            return {};
        }
        return failure(CallableError {.code = 9});
    }

    Result<int&, CallableError> result_ref(bool succeed) {
        if (succeed) {
            return value;
        }
        return failure(CallableError {.code = 11});
    }
};

struct PointerCtorFixture {
    int* ptr {nullptr};

    explicit PointerCtorFixture(int* ptr) : ptr(ptr) {}
};

struct MoveOnlyArgument {
    int value {0};

    explicit MoveOnlyArgument(int value) : value(value) {}
    MoveOnlyArgument(const MoveOnlyArgument&) = delete;
    MoveOnlyArgument& operator=(const MoveOnlyArgument&) = delete;
    MoveOnlyArgument(MoveOnlyArgument&&) = default;
    MoveOnlyArgument& operator=(MoveOnlyArgument&&) = default;
};

struct ConstOverloadFixture {
    int read() { return 1; }
    int read() const { return 2; }
};

struct NumericOverloadFixture {
    int selected {0};

    void choose(float) { selected = 1; }
    void choose(double) { selected = 2; }
};

struct NumericConstructorFixture {
    int selected {0};

    explicit NumericConstructorFixture(float) : selected(1) {}
    explicit NumericConstructorFixture(double) : selected(2) {}
};

struct ImplicitTextFixture {
    std::string value;

    ImplicitTextFixture(const std::string& value) : value(value) {}
};

struct ExplicitTextFixture {
    std::string value;

    explicit ExplicitTextFixture(const std::string& value) : value(value) {}
};

struct AmbiguousConversionFixture {
    int selected {0};

    AmbiguousConversionFixture(float) : selected(1) {}
    AmbiguousConversionFixture(double) : selected(2) {}
};

struct ConvertingArgumentFixture {
    std::size_t implicit_length(const ImplicitTextFixture& text) const {
        return text.value.size();
    }

    std::size_t explicit_length(const ExplicitTextFixture& text) const {
        return text.value.size();
    }

    int ambiguous_value(const AmbiguousConversionFixture& value) const {
        return value.selected;
    }

    int choose(const std::string&) const { return 1; }
    int choose(const ImplicitTextFixture&) const { return 2; }
};

struct LookupFixture {
    int value {0};

    explicit LookupFixture(int value) : value(value) {}

    void set_value(int next) { value = next; }
};

} // namespace

TEST_CASE(
    "Property validates object constness and value types",
    "[refl][property]"
) {
    auto& cls =
        Registry::instance().register_cls<CallableFixture>().add_property(
            "value",
            &CallableFixture::value
        );
    auto& prop = cls.get_property("value");

    CallableFixture obj {3};
    int next = 9;
    float wrong = 2.0f;

    auto initial_value = prop.get(make_ref(obj));
    REQUIRE(initial_value);
    REQUIRE(initial_value->get<int>() == 3);
    REQUIRE(prop.set(make_ref(obj), make_ref(next)));
    REQUIRE(obj.value == 9);

    auto wrong_value = prop.set(make_ref(obj), make_ref(wrong));
    REQUIRE_FALSE(wrong_value);
    REQUIRE(wrong_value.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        wrong_value.error().message.find("expected int") != std::string::npos
    );
    REQUIRE(wrong_value.error().message.find("got float") != std::string::npos);

    const CallableFixture const_obj {7};
    auto const_value = prop.get(make_ref(const_obj));
    REQUIRE(const_value);
    REQUIRE(const_value->is_const());
    REQUIRE(const_value->get_const<int>() == 7);

    auto const_set = prop.set(make_ref(const_obj), make_ref(next));
    REQUIRE_FALSE(const_set);
    REQUIRE(const_set.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        const_set.error().message.find("Invalid or const object") !=
        std::string::npos
    );

    auto invalid_get = prop.get(make_ref(wrong));
    REQUIRE_FALSE(invalid_get);
    REQUIRE(invalid_get.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        invalid_get.error().message.find("Invalid object") != std::string::npos
    );
}

TEST_CASE("Property accepts runtime enum values", "[refl][property]") {
    auto& registry = Registry::instance();
    auto& enm = registry.register_enum<TestEnum>();
    auto& cls = registry.register_cls<EnumPropertyFixture>().add_property(
        "value",
        &EnumPropertyFixture::value
    );
    auto& prop = cls.get_property("value");

    EnumPropertyFixture obj;
    auto value = enm.make_val(2);

    REQUIRE(value.type_id() == type_id<TestEnum>());
    REQUIRE(prop.set(make_ref(obj), value.ref()));
    REQUIRE(obj.value == TestEnum::Two);
}

TEST_CASE("Property set applies safe value conversions", "[refl][property]") {
    auto& cls =
        Registry::instance()
            .register_cls<PropertyConversionFixture>()
            .add_property("scale", &PropertyConversionFixture::scale)
            .add_property("precise", &PropertyConversionFixture::precise)
            .add_property("value", &PropertyConversionFixture::value)
            .add_property("mode", &PropertyConversionFixture::mode)
            .add_property("view", &PropertyConversionFixture::view);

    auto& scale = cls.get_property("scale");
    auto& precise = cls.get_property("precise");
    auto& value = cls.get_property("value");
    auto& mode = cls.get_property("mode");
    auto& view = cls.get_property("view");

    PropertyConversionFixture obj;
    int integer = 4;
    int raw_enum = 2;
    float real = 2.5f;
    double too_precise = 2.5;
    std::string text = "hello";

    REQUIRE(scale.set(make_ref(obj), make_ref(integer)));
    REQUIRE(obj.scale == 4.0f);

    REQUIRE(precise.set(make_ref(obj), make_ref(real)));
    REQUIRE(obj.precise == 2.5);

    REQUIRE(mode.set(make_ref(obj), make_ref(raw_enum)));
    REQUIRE(obj.mode == TestEnum::Two);

    auto int_set = value.set(make_ref(obj), make_ref(real));
    REQUIRE_FALSE(int_set);
    REQUIRE(int_set.error().message.find("expected int") != std::string::npos);
    REQUIRE(obj.value == 0);

    auto scale_set = scale.set(make_ref(obj), make_ref(too_precise));
    REQUIRE_FALSE(scale_set);
    REQUIRE(
        scale_set.error().message.find("expected float") != std::string::npos
    );
    REQUIRE(obj.scale == 4.0f);

    auto view_set = view.set(make_ref(obj), make_ref(text));
    REQUIRE_FALSE(view_set);
    REQUIRE(
        view_set.error().message.find("property set view") != std::string::npos
    );
    REQUIRE(obj.view.empty());
}

TEST_CASE("ArgumentAdapter exposes Ref parameter conversions", "[refl][args]") {
    int value = 42;
    auto ref = make_ref(value);

    static_assert(
        std::is_same_v<decltype(ArgumentAdapter<int>::get(ref)), int>
    );
    static_assert(
        std::is_same_v<decltype(ArgumentAdapter<int&>::get(ref)), int&>
    );

    REQUIRE(ArgumentAdapter<int>::accepts(ref));
    REQUIRE(ArgumentAdapter<int&>::accepts(ref));
    REQUIRE(ArgumentAdapter<const int&>::accepts(ref));
    REQUIRE(ArgumentAdapter<int*>::accepts(ref));
    REQUIRE(ArgumentAdapter<const int*>::accepts(ref));

    REQUIRE(ArgumentAdapter<int>::get(ref) == 42);
    ArgumentAdapter<int&>::get(ref) = 7;
    REQUIRE(value == 7);
    REQUIRE(ArgumentAdapter<const int&>::get(ref) == 7);
    REQUIRE(ArgumentAdapter<int*>::get(ref) == &value);

    const int const_value = 9;
    auto const_ref = make_ref(const_value);

    REQUIRE(ArgumentAdapter<int>::accepts(const_ref));
    REQUIRE(ArgumentAdapter<const int&>::accepts(const_ref));
    REQUIRE(ArgumentAdapter<const int*>::accepts(const_ref));
    REQUIRE_FALSE(ArgumentAdapter<int&>::accepts(const_ref));
    REQUIRE_FALSE(ArgumentAdapter<int*>::accepts(const_ref));
    REQUIRE(ArgumentAdapter<int>::get(const_ref) == 9);
    REQUIRE(ArgumentAdapter<const int*>::get(const_ref) == &const_value);
}

TEST_CASE(
    "ArgumentAdapter preserves enum and move-only rules",
    "[refl][args]"
) {
    int raw_enum = 2;
    auto enum_ref = make_ref(raw_enum);

    REQUIRE(ArgumentAdapter<TestEnum>::accepts(enum_ref));
    REQUIRE(ArgumentAdapter<const TestEnum&>::accepts(enum_ref));
    REQUIRE_FALSE(ArgumentAdapter<TestEnum&>::accepts(enum_ref));
    REQUIRE_FALSE(ArgumentAdapter<TestEnum*>::accepts(enum_ref));
    REQUIRE(ArgumentAdapter<TestEnum>::get(enum_ref) == TestEnum::Two);
    REQUIRE(ArgumentAdapter<const TestEnum&>::get(enum_ref) == TestEnum::Two);

    MoveOnlyArgument move_only {11};
    auto move_ref = make_ref(move_only);

    static_assert(std::is_same_v<
                  decltype(ArgumentAdapter<MoveOnlyArgument>::get(move_ref)),
                  MoveOnlyArgument&&>);

    REQUIRE(ArgumentAdapter<MoveOnlyArgument>::accepts(move_ref));
    REQUIRE(ArgumentAdapter<MoveOnlyArgument&>::get(move_ref).value == 11);
    MoveOnlyArgument moved {ArgumentAdapter<MoveOnlyArgument>::get(move_ref)};
    REQUIRE(moved.value == 11);

    const MoveOnlyArgument const_move_only {13};
    auto const_move_ref = make_ref(const_move_only);

    REQUIRE_FALSE(ArgumentAdapter<MoveOnlyArgument>::accepts(const_move_ref));
    REQUIRE_FALSE(ArgumentAdapter<MoveOnlyArgument&>::accepts(const_move_ref));
    REQUIRE(ArgumentAdapter<const MoveOnlyArgument&>::accepts(const_move_ref));
    REQUIRE(
        ArgumentAdapter<const MoveOnlyArgument&>::get(const_move_ref).value ==
        13
    );
}

TEST_CASE(
    "ArgumentAdapter applies conservative weak conversions",
    "[refl][args]"
) {
    short small_integer = 3;
    int integer = 4;
    float real = 2.5f;
    double precise = 2.5;
    bool flag = true;
    std::string text = "hello";

    REQUIRE(ArgumentAdapter<int>::accepts(make_ref(small_integer)));
    REQUIRE(ArgumentAdapter<int>::get(make_ref(small_integer)) == 3);

    REQUIRE(ArgumentAdapter<float>::accepts(make_ref(integer)));
    REQUIRE(ArgumentAdapter<float>::get(make_ref(integer)) == 4.0f);

    REQUIRE(ArgumentAdapter<double>::accepts(make_ref(real)));
    REQUIRE(ArgumentAdapter<double>::get(make_ref(real)) == 2.5);

    REQUIRE_FALSE(ArgumentAdapter<int>::accepts(make_ref(real)));
    REQUIRE_FALSE(ArgumentAdapter<float>::accepts(make_ref(precise)));
    REQUIRE_FALSE(ArgumentAdapter<int>::accepts(make_ref(flag)));

    REQUIRE(ArgumentAdapter<std::string_view>::accepts(make_ref(text)));
    REQUIRE(ArgumentAdapter<std::string_view>::get(make_ref(text)) == "hello");
    REQUIRE_FALSE(ArgumentAdapter<std::string_view&>::accepts(make_ref(text)));
}

TEST_CASE("Method accepts weakly converted arguments", "[refl][method]") {
    auto& cls = Registry::instance()
                    .register_cls<CallableFixture>()
                    .add_method("scale_float", &CallableFixture::scale_float)
                    .add_method("add_double", &CallableFixture::add_double)
                    .add_method("text_size", &CallableFixture::text_size);

    CallableFixture obj {5};
    int integer = 2;
    float real = 2.5f;
    std::string text = "hello";

    auto& scale_float = cls.get_method("scale_float", {type_id<float>()});
    auto scale_ret = scale_float.invoke(make_ref(obj), make_ref(integer));
    REQUIRE(scale_ret);
    REQUIRE(scale_ret->is_value());
    REQUIRE(scale_ret->value().get<float>() == 10.0f);

    auto& add_double = cls.get_method("add_double", {type_id<double>()});
    auto double_ret = add_double.invoke(make_ref(obj), make_ref(real));
    REQUIRE(double_ret);
    REQUIRE(double_ret->is_value());
    REQUIRE(double_ret->value().get<double>() == 7.5);

    auto& text_size =
        cls.get_method("text_size", {type_id<std::string_view>()});
    auto text_ret = text_size.invoke(make_ref(obj), make_ref(text));
    REQUIRE(text_ret);
    REQUIRE(text_ret->is_value());
    REQUIRE(text_ret->value().get<std::size_t>() == 5);
}

TEST_CASE(
    "Method resolution prefers exact matches and rejects ambiguity",
    "[refl][method]"
) {
    auto& cls = Registry::instance()
                    .register_cls<NumericOverloadFixture>()
                    .add_method(
                        "choose",
                        static_cast<void (NumericOverloadFixture::*)(float)>(
                            &NumericOverloadFixture::choose
                        )
                    )
                    .add_method(
                        "choose",
                        static_cast<void (NumericOverloadFixture::*)(double)>(
                            &NumericOverloadFixture::choose
                        )
                    );

    NumericOverloadFixture obj;
    float real = 2.5f;
    int integer = 2;

    auto exact = cls.get_method_for_args(
        "choose",
        {make_ref(obj), make_ref(real)},
        MethodConstFilter::PreferNonConst
    );
    REQUIRE(exact);
    auto exact_ret = exact->invoke_variadic({make_ref(obj), make_ref(real)});
    REQUIRE(exact_ret);
    REQUIRE(obj.selected == 1);

    auto ambiguous = cls.get_method_for_args(
        "choose",
        {make_ref(obj), make_ref(integer)},
        MethodConstFilter::PreferNonConst
    );
    REQUIRE_FALSE(ambiguous);
    REQUIRE(ambiguous.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        ambiguous.error().message.find("Ambiguous method") != std::string::npos
    );
}

TEST_CASE("Method validates instances and argument types", "[refl][method]") {
    auto& registry = Registry::instance();
    auto& enm = registry.register_enum<TestEnum>();
    auto& cls = registry.register_cls<CallableFixture>()
                    .add_method("add", &CallableFixture::add)
                    .add_method("read", &CallableFixture::read)
                    .add_method("set", &CallableFixture::set)
                    .add_method("sum", &CallableFixture::sum)
                    .add_method("enum_value", &CallableFixture::enum_value)
                    .add_method("read_ptr", &CallableFixture::read_ptr)
                    .add_method("write_ptr", &CallableFixture::write_ptr);

    auto method_count = cls.get_methods("add").size();
    cls.add_method("add", &CallableFixture::add);
    REQUIRE(cls.get_methods("add").size() == method_count);

    CallableFixture obj {5};
    int rhs = 4;
    float wrong = 4.0f;

    auto& add = cls.get_method("add", {type_id<int>()});
    auto add_ret = add.invoke(make_ref(obj), make_ref(rhs));
    REQUIRE(add_ret);
    REQUIRE(add_ret->is_value());
    REQUIRE(add_ret->value().get<int>() == 9);

    auto wrong_ret = add.invoke(make_ref(obj), make_ref(wrong));
    REQUIRE_FALSE(wrong_ret);
    REQUIRE(wrong_ret.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        wrong_ret.error().message.find("Invalid argument 1 for method add") !=
        std::string::npos
    );
    REQUIRE(wrong_ret.error().message.find("expected") != std::string::npos);
    REQUIRE(wrong_ret.error().message.find("got float") != std::string::npos);

    const CallableFixture const_obj {8};
    auto& read = cls.get_method("read", {});
    auto read_ret = read.invoke(make_ref(const_obj));
    REQUIRE(read_ret);
    REQUIRE(read_ret->is_value());
    REQUIRE(read_ret->value().get<int>() == 8);

    auto const_add_ret = add.invoke(make_ref(const_obj), make_ref(rhs));
    REQUIRE_FALSE(const_add_ret);
    REQUIRE(const_add_ret.error().kind == InvokeFailure::Kind::InvalidCall);

    int lhs = 2;
    auto& sum = cls.get_method("sum", {type_id<int>(), type_id<int>()});
    auto sum_ret = sum.invoke(make_ref(lhs), make_ref(rhs));
    REQUIRE(sum_ret);
    REQUIRE(sum_ret->is_value());
    REQUIRE(sum_ret->value().get<int>() == 6);

    auto missing_enum_method =
        cls.try_get_method("enum_value", {type_id<int>()});
    REQUIRE_FALSE(missing_enum_method);
    REQUIRE(missing_enum_method.error().kind == ClsError::Kind::MethodNotFound);
    auto& enum_method = cls.get_method("enum_value", {type_id<TestEnum>()});
    auto enum_arg = enm.make_val(2);
    REQUIRE(enum_arg.type_id() == type_id<TestEnum>());
    auto enum_ret = enum_method.invoke(make_ref(obj), enum_arg.ref());
    REQUIRE(enum_ret);
    REQUIRE(enum_ret->is_value());
    REQUIRE(enum_ret->value().get<int>() == 2);

    int pointed = 11;
    auto& read_ptr = cls.get_method("read_ptr", {type_id<int>()});
    auto read_ptr_ret = read_ptr.invoke(make_ref(obj), make_ref(pointed));
    REQUIRE(read_ptr_ret);
    REQUIRE(read_ptr_ret->is_value());
    REQUIRE(read_ptr_ret->value().get<int>() == 11);

    auto& write_ptr = cls.get_method("write_ptr", {type_id<int>()});
    auto write_ptr_ret = write_ptr.invoke(make_ref(obj), make_ref(pointed));
    REQUIRE(write_ptr_ret);
    REQUIRE(write_ptr_ret->is_void());
    REQUIRE(pointed == 5);

    const int const_pointed = 13;
    auto const_write_ret =
        write_ptr.invoke(make_ref(obj), make_ref(const_pointed));
    REQUIRE_FALSE(const_write_ret);
    REQUIRE(const_write_ret.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        const_write_ret.error().message.find(
            "Invalid argument 1 for method write_ptr"
        ) != std::string::npos
    );
    REQUIRE(
        const_write_ret.error().message.find("got const int") !=
        std::string::npos
    );
    REQUIRE(pointed == 5);
}

TEST_CASE("Method adapts Result and Status returns", "[refl][method]") {
    auto& cls = Registry::instance()
                    .register_cls<ResultFixture>()
                    .add_method("result_value", &ResultFixture::result_value)
                    .add_method("status_value", &ResultFixture::status_value)
                    .add_method("result_ref", &ResultFixture::result_ref);

    ResultFixture obj;
    bool succeed = true;
    bool fail = false;

    auto& result_method = cls.get_method("result_value", {type_id<bool>()});

    auto result_ok = result_method.invoke(make_ref(obj), make_ref(succeed));
    REQUIRE(result_ok);
    REQUIRE(result_ok->is_value());
    REQUIRE(result_ok->value().get<int>() == 42);

    auto result_err = result_method.invoke(make_ref(obj), make_ref(fail));
    REQUIRE_FALSE(result_err);
    REQUIRE(result_err.error().kind == InvokeFailure::Kind::ReturnedError);
    REQUIRE(result_err.error().error.get<CallableError>().code == 7);

    auto& status_method = cls.get_method("status_value", {type_id<bool>()});

    auto status_ok = status_method.invoke(make_ref(obj), make_ref(succeed));
    REQUIRE(status_ok);
    REQUIRE(status_ok->is_status());

    auto status_err = status_method.invoke(make_ref(obj), make_ref(fail));
    REQUIRE_FALSE(status_err);
    REQUIRE(status_err.error().kind == InvokeFailure::Kind::ReturnedError);
    REQUIRE(status_err.error().error.get<CallableError>().code == 9);

    auto& ref_method = cls.get_method("result_ref", {type_id<bool>()});

    auto ref_ok = ref_method.invoke(make_ref(obj), make_ref(succeed));
    REQUIRE(ref_ok);
    REQUIRE(ref_ok->is_ref());
    REQUIRE(&ref_ok->ref().get<int>() == &obj.value);
    ref_ok->ref().get<int>() = 17;
    REQUIRE(obj.value == 17);

    auto ref_err = ref_method.invoke(make_ref(obj), make_ref(fail));
    REQUIRE_FALSE(ref_err);
    REQUIRE(ref_err.error().kind == InvokeFailure::Kind::ReturnedError);
    REQUIRE(ref_err.error().error.get<CallableError>().code == 11);
}

TEST_CASE("Method lookup filters const overloads", "[refl][method]") {
    auto& cls = Registry::instance()
                    .register_cls<ConstOverloadFixture>()
                    .add_method(
                        "read",
                        static_cast<int (ConstOverloadFixture::*)()>(
                            &ConstOverloadFixture::read
                        )
                    )
                    .add_method(
                        "read",
                        static_cast<int (ConstOverloadFixture::*)() const>(
                            &ConstOverloadFixture::read
                        )
                    );

    REQUIRE(cls.get_methods("read").size() == 2);

    auto& any = cls.get_method("read", {}, MethodConstFilter::Any);
    auto& non_const =
        cls.get_method("read", {}, MethodConstFilter::NonConstOnly);
    auto& const_only = cls.get_method("read", {}, MethodConstFilter::ConstOnly);
    auto& prefer_non_const =
        cls.get_method("read", {}, MethodConstFilter::PreferNonConst);
    auto& prefer_const =
        cls.get_method("read", {}, MethodConstFilter::PreferConst);

    REQUIRE(&any == &non_const);
    REQUIRE(&non_const != &const_only);
    REQUIRE(&prefer_non_const == &non_const);
    REQUIRE(&prefer_const == &const_only);

    ConstOverloadFixture obj;
    const ConstOverloadFixture const_obj;

    auto non_const_ret = non_const.invoke(make_ref(obj));
    REQUIRE(non_const_ret);
    REQUIRE(non_const_ret->value().get<int>() == 1);

    auto non_const_on_const_ret = non_const.invoke(make_ref(const_obj));
    REQUIRE_FALSE(non_const_on_const_ret);
    REQUIRE(
        non_const_on_const_ret.error().kind == InvokeFailure::Kind::InvalidCall
    );

    auto const_from_mut_ret = const_only.invoke(make_ref(obj));
    REQUIRE(const_from_mut_ret);
    REQUIRE(const_from_mut_ret->value().get<int>() == 2);

    auto const_from_const_ret = const_only.invoke(make_ref(const_obj));
    REQUIRE(const_from_const_ret);
    REQUIRE(const_from_const_ret->value().get<int>() == 2);
}

TEST_CASE(
    "Constructor resolution prefers exact matches and rejects ambiguity",
    "[refl][constructor]"
) {
    auto& cls = Registry::instance()
                    .register_cls<NumericConstructorFixture>()
                    .add_constructor<NumericConstructorFixture, float>()
                    .add_constructor<NumericConstructorFixture, double>();

    float real = 2.5f;
    int integer = 2;

    auto exact = cls.get_constructor_for_args({make_ref(real)});
    REQUIRE(exact);
    auto exact_ret = exact->invoke_variadic({make_ref(real)});
    REQUIRE(exact_ret);
    REQUIRE(exact_ret->value().get<NumericConstructorFixture>().selected == 1);

    auto ambiguous = cls.get_constructor_for_args({make_ref(integer)});
    REQUIRE_FALSE(ambiguous);
    REQUIRE(ambiguous.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        ambiguous.error().message.find("Ambiguous constructor") !=
        std::string::npos
    );
}

TEST_CASE(
    "Reflected calls apply non-explicit converting constructors",
    "[refl][constructor][conversion]"
) {
    auto& registry = Registry::instance();
    auto& implicit_cls =
        registry.register_cls<ImplicitTextFixture>()
            .add_constructor<ImplicitTextFixture, const std::string&>();
    auto& explicit_cls =
        registry.register_cls<ExplicitTextFixture>()
            .add_constructor<ExplicitTextFixture, const std::string&>();
    registry.register_cls<AmbiguousConversionFixture>()
        .add_constructor<AmbiguousConversionFixture, float>()
        .add_constructor<AmbiguousConversionFixture, double>();

    REQUIRE(
        implicit_cls.get_constructor({type_id<std::string>()}).is_converting()
    );
    REQUIRE_FALSE(
        explicit_cls.get_constructor({type_id<std::string>()}).is_converting()
    );

    auto& cls = registry.register_cls<ConvertingArgumentFixture>()
                    .add_method(
                        "implicit_length",
                        &ConvertingArgumentFixture::implicit_length
                    )
                    .add_method(
                        "explicit_length",
                        &ConvertingArgumentFixture::explicit_length
                    )
                    .add_method(
                        "ambiguous_value",
                        &ConvertingArgumentFixture::ambiguous_value
                    )
                    .add_method(
                        "choose",
                        static_cast<int (ConvertingArgumentFixture::*)(
                            const std::string&
                        ) const>(&ConvertingArgumentFixture::choose)
                    )
                    .add_method(
                        "choose",
                        static_cast<int (ConvertingArgumentFixture::*)(
                            const ImplicitTextFixture&
                        ) const>(&ConvertingArgumentFixture::choose)
                    );

    const ConvertingArgumentFixture instance;
    std::string text = "hello";

    auto implicit = cls.get_method_for_args(
        "implicit_length",
        {make_ref(instance), make_ref(text)}
    );
    REQUIRE(implicit);
    auto implicit_result = implicit->invoke(make_ref(instance), make_ref(text));
    REQUIRE(implicit_result);
    REQUIRE(implicit_result->value().get<std::size_t>() == text.size());

    auto explicit_method = cls.get_method_for_args(
        "explicit_length",
        {make_ref(instance), make_ref(text)}
    );
    REQUIRE_FALSE(explicit_method);

    float ambiguous_source = 1.0f;
    auto ambiguous = cls.get_method_for_args(
        "ambiguous_value",
        {make_ref(instance), make_ref(ambiguous_source)}
    );
    REQUIRE(ambiguous);

    int ambiguous_integer = 1;
    ambiguous = cls.get_method_for_args(
        "ambiguous_value",
        {make_ref(instance), make_ref(ambiguous_integer)}
    );
    REQUIRE_FALSE(ambiguous);

    auto exact =
        cls.get_method_for_args("choose", {make_ref(instance), make_ref(text)});
    REQUIRE(exact);
    auto exact_result = exact->invoke(make_ref(instance), make_ref(text));
    REQUIRE(exact_result);
    REQUIRE(exact_result->value().get<int>() == 1);
}

TEST_CASE("Constructor validates argument types", "[refl][constructor]") {
    auto& cls = Registry::instance()
                    .register_cls<CallableFixture>()
                    .add_constructor<CallableFixture, int>();

    int value = 12;
    float wrong = 12.0f;

    auto& ctor = cls.get_constructor({type_id<int>()});

    auto ret = ctor.invoke_variadic({make_ref(value)});
    REQUIRE(ret);
    REQUIRE(ret->is_value());
    REQUIRE(ret->value().get<CallableFixture>().value == 12);

    auto wrong_ret = ctor.invoke_variadic({make_ref(wrong)});
    REQUIRE_FALSE(wrong_ret);
    REQUIRE(wrong_ret.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        wrong_ret.error().message.find("Invalid argument 1 for constructor") !=
        std::string::npos
    );
    REQUIRE(wrong_ret.error().message.find("got float") != std::string::npos);

    auto& ptr_cls = Registry::instance()
                        .register_cls<PointerCtorFixture>()
                        .add_constructor<PointerCtorFixture, int*>();
    auto& ptr_ctor = ptr_cls.get_constructor({type_id<int>()});

    int pointed = 7;
    auto ptr_ret = ptr_ctor.invoke_variadic({make_ref(pointed)});
    REQUIRE(ptr_ret);
    REQUIRE(ptr_ret->is_value());
    REQUIRE(ptr_ret->value().get<PointerCtorFixture>().ptr == &pointed);

    const int const_pointed = 9;
    auto const_ptr_ret = ptr_ctor.invoke_variadic({make_ref(const_pointed)});
    REQUIRE_FALSE(const_ptr_ret);
    REQUIRE(const_ptr_ret.error().kind == InvokeFailure::Kind::InvalidCall);
    REQUIRE(
        const_ptr_ret.error().message.find(
            "Invalid argument 1 for constructor"
        ) != std::string::npos
    );
    REQUIRE(
        const_ptr_ret.error().message.find("got const int") != std::string::npos
    );
}

TEST_CASE("Cls try_get reports missing members", "[refl][cls]") {
    Registry::instance().register_type<float>();

    auto& cls = Registry::instance()
                    .register_cls<LookupFixture>()
                    .add_property("value", &LookupFixture::value)
                    .add_method("set_value", &LookupFixture::set_value)
                    .add_constructor<LookupFixture, int>();

    auto property = cls.try_get_property("missing");
    REQUIRE_FALSE(property);
    REQUIRE(property.error().kind == ClsError::Kind::PropertyNotFound);
    REQUIRE(property.error().owner_type_id == type_id<LookupFixture>());
    REQUIRE(property.error().owner_type_name);
    REQUIRE(property.error().member_name == "missing");
    REQUIRE(
        property.error().message.find("LookupFixture") != std::string::npos
    );

    auto method = cls.try_get_method("set_value", {type_id<float>()});
    REQUIRE_FALSE(method);
    REQUIRE(method.error().kind == ClsError::Kind::MethodNotFound);
    REQUIRE(method.error().member_name == "set_value");
    REQUIRE(method.error().arg_types == std::vector<TypeId> {type_id<float>()});
    REQUIRE(method.error().message.find("float") != std::string::npos);

    auto constructor = cls.try_get_constructor({type_id<float>()});
    REQUIRE_FALSE(constructor);
    REQUIRE(constructor.error().kind == ClsError::Kind::ConstructorNotFound);
    REQUIRE(
        constructor.error().arg_types == std::vector<TypeId> {type_id<float>()}
    );
    REQUIRE(
        constructor.error().message.find("LookupFixture") != std::string::npos
    );
}

TEST_CASE(
    "Const erased returns retain the payload and return qualification",
    "[refl][callable]"
) {
    const MethodImpl method(
        "snapshot",
        +[]() -> const Val {
            return make_val<std::string>("snapshot");
        }
    );
    CHECK(method.return_type().is_const());
    auto result = method.invoke_variadic({});
    REQUIRE(result);
    REQUIRE(result->is_value());
    CHECK(result->value().type_id() == type_id<std::string>());
    CHECK(result->value().get<std::string>() == "snapshot");
}
