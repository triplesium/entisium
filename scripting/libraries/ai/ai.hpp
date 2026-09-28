#pragma once
#include "scripting/annotations.hpp" // IWYU pragma: keep
#include "scripting/library.hpp"
namespace ets::luau::ai {
ETS_REFLECT()
struct Model {
    std::string id, protocol, connection;
    double timeout;
    int max_tokens;
};
ETS_REFLECT(LuauLibrary(name = "@ai"))
class Library final {
  public:
    explicit Library(LuauLibraryServices& services) :
        m_config(services.config) {}
    Model model(std::string kind, std::string alias) const;

  private:
    std::shared_ptr<const LuauHostConfig> m_config;
};
} // namespace ets::luau::ai
