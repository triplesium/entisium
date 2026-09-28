#include "scripting/host_config.hpp"

#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace ets {
std::shared_ptr<const LuauHostConfig>
LuauHostConfig::parse(std::string_view text) {
    try {
        if (text.size() > 32ULL * 1024 * 1024) {
            throw std::invalid_argument("size");
        }
        auto data = nlohmann::json::parse(text);
        if (data.at("version") != 1) {
            throw std::invalid_argument("version");
        }
        auto result = std::make_shared<LuauHostConfig>();
        const auto& connections = data.at("connections");
        if (!connections.is_object() || connections.size() > 256) {
            throw std::invalid_argument("connections");
        }
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            LuauConnectionConfig connection;
            connection.base_url = it.value().at("base_url").get<std::string>();
            const auto& url = connection.base_url;
            const auto scheme = url.find("://");
            if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
                url.size() > 4096 ||
                url.find_first_of("?#@\\\r\n\t ") != std::string::npos ||
                url.size() <= scheme + 3) {
                throw std::invalid_argument("base URL");
            }
            connection.error = it.value().value("error", "");
            const auto& headers = it.value().at("headers");
            if (!headers.is_array() || headers.size() > 32) {
                throw std::invalid_argument("headers");
            }
            for (const auto& header : headers) {
                auto name = header.at("name").get<std::string>();
                auto value = header.at("value").get<std::string>();
                if (name.empty() || name.size() > 128 || value.size() > 8192 ||
                    name.find_first_of("\r\n:") != std::string::npos ||
                    value.find_first_of("\r\n") != std::string::npos ||
                    name.find('\0') != std::string::npos ||
                    value.find('\0') != std::string::npos) {
                    throw std::invalid_argument("header");
                }
                connection.headers.emplace_back(
                    std::move(name),
                    std::move(value)
                );
            }
            result->connections.emplace(it.key(), std::move(connection));
        }
        for (const auto* kind : {"generation", "decisions"}) {
            const auto& models = data.at(kind);
            if (!models.is_object() || models.size() > 1024) {
                throw std::invalid_argument("models");
            }
            auto& destination = std::string_view(kind) == "generation" ?
                                    result->generation :
                                    result->decisions;
            for (auto it = models.begin(); it != models.end(); ++it) {
                LuauModelConfig model;
                model.id = it.value().at("id").get<std::string>();
                model.protocol = it.value().at("protocol").get<std::string>();
                model.connection =
                    it.value().at("connection").get<std::string>();
                model.timeout = it.value().at("timeout").get<double>();
                model.max_tokens = it.value().value("max_tokens", 4096);
                if (model.id.empty() || model.id.size() > 256 ||
                    !result->connections.contains(model.connection) ||
                    !std::isfinite(model.timeout) || model.timeout <= 0 ||
                    model.timeout > 300 || model.max_tokens < 1 ||
                    model.max_tokens > 65536 ||
                    (std::string_view(kind) == "decisions" ?
                         model.protocol != "jev" :
                         (model.protocol != "responses" &&
                          model.protocol != "chat-completions"))) {
                    throw std::invalid_argument("model");
                }
                destination.emplace(it.key(), std::move(model));
            }
        }
        if (data.contains("tape")) {
            result->tape_json = data.at("tape").dump();
        }
        return result;
    } catch (const std::exception&) {
        // Never echo provider configuration or credentials in parse
        // diagnostics.
        throw std::invalid_argument("Invalid Luau host configuration");
    }
}
std::shared_ptr<const LuauHostConfig>
read_luau_host_config(std::istream& input) {
    std::string line;
    char byte = 0;
    while (input.get(byte)) {
        if (byte == '\n') {
            return LuauHostConfig::parse(line);
        }
        if (line.size() >= 32ULL * 1024 * 1024) {
            throw std::invalid_argument(
                "Luau host configuration exceeds 32 MiB"
            );
        }
        line += byte;
    }
    throw std::invalid_argument("Missing Luau host configuration envelope");
}
} // namespace ets
