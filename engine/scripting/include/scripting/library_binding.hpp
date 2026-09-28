#pragma once
#include "refl/val.hpp"
#include "scripting/library.hpp"

#include <memory>
#include <type_traits>
namespace ets {
template<class T>
class LuauLibraryBinding : public LuauNativeLibrary {
  protected:
    std::shared_ptr<Val> m_instance;

  public:
    explicit LuauLibraryBinding(LuauLibraryServices& services) {
        if constexpr (std::is_constructible_v<T, LuauLibraryServices&>) {
            m_instance = std::make_shared<Val>(make_val<T>(services));
        } else if constexpr (std::is_default_constructible_v<T>) {
            m_instance = std::make_shared<Val>(make_val<T>());
        }
    }
    void cancel() noexcept override {
        if (!m_instance) {
            return;
        }
        if constexpr (requires(T& value) { value.cancel(); }) {
            m_instance->get<T>().cancel();
        }
    }
    std::size_t pending() const override {
        if (!m_instance) {
            return 0;
        }
        if constexpr (requires(const T& value) { value.pending(); }) {
            return m_instance->get<T>().pending();
        }
        return 0;
    }
    std::string diagnostic(std::string_view key) const override {
        if (!m_instance) {
            return {};
        }
        if constexpr (requires(const T& value) { value.diagnostic(key); }) {
            return m_instance->get<T>().diagnostic(key);
        }
        return {};
    }
};
} // namespace ets
