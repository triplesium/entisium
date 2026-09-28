#pragma once

#include "base/optional.hpp"
#include "refl/containers/associative.hpp"
#include "refl/containers/product.hpp"
#include "refl/containers/sequence.hpp"
#include "refl/generic_type.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ets {

template<class T, class Alloc>
struct GenericTypeInfo<std::vector<T, Alloc>> {
    static constexpr bool supported = !std::same_as<T, bool> && std::movable<T>;
    using Container = std::vector<T, Alloc>;
    using Dependencies = std::tuple<T>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::vector"});
    }

    static std::string generic_name() { return "std::vector"; }

    static std::vector<TypeId> argument_type_ids() { return {type_id<T>()}; }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<VectorContainerAdapter<Container, T>>();
    }
};

template<class T, std::size_t Size>
struct GenericTypeInfo<std::array<T, Size>> {
    static constexpr bool supported = true;
    using Container = std::array<T, Size>;
    using Dependencies = std::tuple<T>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::array"});
    }

    static std::string generic_name() { return "std::array"; }

    static std::vector<TypeId> argument_type_ids() { return {type_id<T>()}; }

    static std::vector<GenericArgument> arguments() {
        return {
            GenericArgument::type(type_id<T>()),
            GenericArgument::unsigned_value(Size),
        };
    }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<ArrayContainerAdapter<Container, T, Size>>();
    }
};

template<class Key, class Mapped, class Compare, class Alloc>
struct GenericTypeInfo<std::map<Key, Mapped, Compare, Alloc>> {
    static constexpr bool supported = std::movable<Key> && std::movable<Mapped>;
    using Container = std::map<Key, Mapped, Compare, Alloc>;
    using Dependencies =
        std::tuple<Key, Mapped, typename Container::value_type>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::map"});
    }

    static std::string generic_name() { return "std::map"; }

    static std::vector<TypeId> argument_type_ids() {
        return {type_id<Key>(), type_id<Mapped>()};
    }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<MapContainerAdapter<Container, Key, Mapped>>();
    }
};

template<class Key, class Mapped, class Hash, class Eq, class Alloc>
struct GenericTypeInfo<std::unordered_map<Key, Mapped, Hash, Eq, Alloc>> {
    static constexpr bool supported = std::movable<Key> && std::movable<Mapped>;
    using Container = std::unordered_map<Key, Mapped, Hash, Eq, Alloc>;
    using Dependencies =
        std::tuple<Key, Mapped, typename Container::value_type>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::unordered_map"});
    }

    static std::string generic_name() { return "std::unordered_map"; }

    static std::vector<TypeId> argument_type_ids() {
        return {type_id<Key>(), type_id<Mapped>()};
    }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<MapContainerAdapter<Container, Key, Mapped>>();
    }
};

template<class Key, class Compare, class Alloc>
struct GenericTypeInfo<std::set<Key, Compare, Alloc>> {
    static constexpr bool supported = std::movable<Key>;
    using Container = std::set<Key, Compare, Alloc>;
    using Dependencies = std::tuple<Key>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::set"});
    }

    static std::string generic_name() { return "std::set"; }

    static std::vector<TypeId> argument_type_ids() { return {type_id<Key>()}; }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<SetContainerAdapter<Container, Key>>();
    }
};

template<class Key, class Hash, class Eq, class Alloc>
struct GenericTypeInfo<std::unordered_set<Key, Hash, Eq, Alloc>> {
    static constexpr bool supported = std::movable<Key>;
    using Container = std::unordered_set<Key, Hash, Eq, Alloc>;
    using Dependencies = std::tuple<Key>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::unordered_set"});
    }

    static std::string generic_name() { return "std::unordered_set"; }

    static std::vector<TypeId> argument_type_ids() { return {type_id<Key>()}; }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<SetContainerAdapter<Container, Key>>();
    }
};

template<class T>
struct GenericTypeInfo<std::optional<T>> {
    static constexpr bool supported = std::movable<T>;
    using Container = std::optional<T>;
    using Dependencies = std::tuple<T>;
    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::optional"});
    }
    static std::string generic_name() { return "std::optional"; }
    static std::vector<TypeId> argument_type_ids() { return {type_id<T>()}; }
    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<OptionalContainerAdapter<Container, T>>();
    }
};

template<class T>
    requires(!std::is_reference_v<T>)
struct GenericTypeInfo<Optional<T>> {
    static constexpr bool supported = true;
    using Container = Optional<T>;
    using Dependencies = std::tuple<T>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"ets::Optional"});
    }

    static std::string generic_name() { return "ets::Optional"; }

    static std::vector<TypeId> argument_type_ids() { return {type_id<T>()}; }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<OptionalContainerAdapter<Container, T>>();
    }
};

template<class First, class Second>
struct GenericTypeInfo<std::pair<First, Second>> {
    static constexpr bool supported = true;
    using Container = std::pair<First, Second>;
    using Dependencies = std::tuple<First, Second>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::pair"});
    }

    static std::string generic_name() { return "std::pair"; }

    static std::vector<TypeId> argument_type_ids() {
        return {type_id<First>(), type_id<Second>()};
    }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<TupleLikeContainerAdapter<Container>>();
    }
};

template<class... Types>
struct GenericTypeInfo<std::tuple<Types...>> {
    static constexpr bool supported = true;
    using Container = std::tuple<Types...>;
    using Dependencies = std::tuple<Types...>;

    static TypeId generic_type_id() {
        return TypeId(std::string_view {"std::tuple"});
    }

    static std::string generic_name() { return "std::tuple"; }

    static std::vector<TypeId> argument_type_ids() {
        return {type_id<Types>()...};
    }

    static std::unique_ptr<ContainerAdapter> make_container_adapter() {
        return std::make_unique<TupleLikeContainerAdapter<Container>>();
    }
};

} // namespace ets
