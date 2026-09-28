#pragma once
#include <istream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace ets {
struct LuauConnectionConfig {
    std::string base_url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
};
struct LuauModelConfig {
    std::string id, protocol, connection;
    double timeout {30};
    int max_tokens {4096};
};
struct LuauHostConfig {
    std::map<std::string, LuauConnectionConfig> connections;
    std::map<std::string, LuauModelConfig> generation, decisions;
    std::string tape_json;
    static std::shared_ptr<const LuauHostConfig> parse(std::string_view text);
};
// One bounded, versioned startup envelope on a private inherited pipe.
std::shared_ptr<const LuauHostConfig>
read_luau_host_config(std::istream& input);
} // namespace ets
