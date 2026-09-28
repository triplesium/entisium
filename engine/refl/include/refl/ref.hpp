#pragma once

#include "base/debug.hpp"
#include "base/log.hpp"
#include "refl/type.hpp"

#include <cassert>
#include <concepts>
#include <type_traits>
#include <utility>

namespace ets {

class Ref {
  private:
    void* m_ptr;
    TypeId m_type_id;
    bool m_is_const;

  public:
    Ref() : m_ptr(nullptr), m_type_id(0), m_is_const(false) {}
    Ref(std::nullptr_t) : Ref() {}
    Ref(void* ptr, TypeId type, bool is_const = false) :
        m_ptr(ptr), m_type_id(type), m_is_const(is_const) {}
    Ref(const void* ptr, TypeId type) :
        m_ptr(const_cast<void*>(ptr)), m_type_id(type), m_is_const(true) {}

    template<typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, Ref>)
    Ref(T& obj) :
        m_ptr(const_cast<std::remove_cv_t<T>*>(&obj)),
        m_type_id(ets::type_id<std::remove_cv_t<T>>()),
        m_is_const(std::is_const_v<T>) {
        static_assert(
            !std::is_pointer_v<std::remove_cvref_t<T>>,
            "Ref cannot be constructed from a pointer type"
        );
    }

    template<class T>
    std::remove_cvref_t<T>* try_get() const {
        using U = std::remove_cvref_t<T>;
        if (!m_ptr || m_is_const || m_type_id != ets::type_id<U>()) {
            return nullptr;
        }
        return static_cast<U*>(m_ptr);
    }

    template<class T>
    const std::remove_cvref_t<T>* try_get_const() const {
        using U = std::remove_cvref_t<T>;
        if (!m_ptr || m_type_id != ets::type_id<U>()) {
            return nullptr;
        }
        return static_cast<const U*>(m_ptr);
    }

    template<class T>
    std::remove_cvref_t<T>& get() const {
        auto* ptr = try_get<T>();
        ETS_ASSERT(ptr);
        return *ptr;
    }

    template<class T>
    const std::remove_cvref_t<T>& get_const() const {
        auto* ptr = try_get_const<T>();
        ETS_ASSERT(ptr);
        return *ptr;
    }

    template<class T>
    std::remove_cvref_t<T>&& get_rref() const {
        auto* ptr = try_get<T>();
        ETS_ASSERT(ptr);
        return std::move(*ptr);
    }

    TypeId type_id() const {
        ETS_ASSERT(m_type_id);
        return m_type_id;
    }

    void* ptr() const { return m_ptr; }
    const void* const_ptr() const { return m_ptr; }
    bool is_const() const { return m_is_const; }

    bool operator==(const Ref& other) const {
        return m_ptr == other.m_ptr && m_type_id == other.m_type_id &&
               m_is_const == other.m_is_const;
    }

    bool operator!=(const Ref& other) const { return !(*this == other); }

    explicit operator bool() const { return m_ptr != nullptr; }

    template<typename T>
        requires std::is_arithmetic_v<T>
    T to_number() const {
        if (m_type_id == ets::type_id<int>()) {
            return static_cast<T>(get_const<int>());
        } else if (m_type_id == ets::type_id<char>()) {
            return static_cast<T>(get_const<char>());
        } else if (m_type_id == ets::type_id<char8_t>()) {
            return static_cast<T>(get_const<char8_t>());
        } else if (m_type_id == ets::type_id<char16_t>()) {
            return static_cast<T>(get_const<char16_t>());
        } else if (m_type_id == ets::type_id<char32_t>()) {
            return static_cast<T>(get_const<char32_t>());
        } else if (m_type_id == ets::type_id<wchar_t>()) {
            return static_cast<T>(get_const<wchar_t>());
        } else if (m_type_id == ets::type_id<signed char>()) {
            return static_cast<T>(get_const<signed char>());
        } else if (m_type_id == ets::type_id<unsigned char>()) {
            return static_cast<T>(get_const<unsigned char>());
        } else if (m_type_id == ets::type_id<short int>()) {
            return static_cast<T>(get_const<short int>());
        } else if (m_type_id == ets::type_id<unsigned short int>()) {
            return static_cast<T>(get_const<unsigned short int>());
        } else if (m_type_id == ets::type_id<unsigned int>()) {
            return static_cast<T>(get_const<unsigned int>());
        } else if (m_type_id == ets::type_id<long int>()) {
            return static_cast<T>(get_const<long int>());
        } else if (m_type_id == ets::type_id<unsigned long int>()) {
            return static_cast<T>(get_const<unsigned long int>());
        } else if (m_type_id == ets::type_id<long long int>()) {
            return static_cast<T>(get_const<long long int>());
        } else if (m_type_id == ets::type_id<unsigned long long int>()) {
            return static_cast<T>(get_const<unsigned long long int>());
        } else if (m_type_id == ets::type_id<bool>()) {
            return static_cast<T>(get_const<bool>());
        } else if (m_type_id == ets::type_id<float>()) {
            return static_cast<T>(get_const<float>());
        } else if (m_type_id == ets::type_id<double>()) {
            return static_cast<T>(get_const<double>());
        } else if (m_type_id == ets::type_id<long double>()) {
            return static_cast<T>(get_const<long double>());
        } else {
            error("Invalid type conversion");
            return static_cast<T>(0);
        }
    }
};

} // namespace ets
