#include "ai.hpp"

#include <stdexcept>
namespace ets::luau::ai {
Model Library::model(std::string kind, std::string alias) const {
    if (!m_config) {
        throw std::runtime_error("Model configuration is missing");
    }
    if (std::string_view(kind) != "generation" &&
        std::string_view(kind) != "decisions") {
        throw std::invalid_argument("Invalid model kind");
    }
    const auto& models = std::string_view(kind) == "generation" ?
                             m_config->generation :
                             m_config->decisions;
    if (models.empty()) {
        throw std::runtime_error(
            std::string(kind) + " model configuration is missing"
        );
    }
    const auto found = models.find(alias);
    if (found == models.end()) {
        throw std::runtime_error(
            std::string("Unknown ") + kind + " model alias: " + alias
        );
    }
    const auto& binding = found->second;
    return {
        binding.id,
        binding.protocol,
        binding.connection,
        binding.timeout,
        binding.max_tokens
    };
}
} // namespace ets::luau::ai
