#include "http.hpp"

#include "base/result.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#ifndef __EMSCRIPTEN__
#    include "http/client.hpp"
#endif
namespace ets::luau::http {
namespace {
Result<std::uint64_t, std::string> request_id(std::string_view text) {
    std::uint64_t id {};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), id);
    if (error != std::errc {} || end != text.data() + text.size()) {
        return failure(std::string("Unknown HTTP request"));
    }
    return id;
}
} // namespace
struct Library::Impl {
    std::shared_ptr<const LuauHostConfig> config;
    struct ConnectionState {
        std::string name;
        Connection handle;
        bool closed {false};
    };
    std::unordered_map<const void*, ConnectionState> connections;
    std::unordered_map<std::string, const void*> handles;
    std::unordered_map<std::string, const void*> requests;
    std::unordered_set<std::string> pending;
    using Json = nlohmann::json;
    Json tape = Json::array();
    std::string tape_mode;
    std::string startup_error;
    std::size_t tape_cursor {0}, tape_bytes {0};
    std::unordered_map<std::string, std::size_t> recorded;
    explicit Impl(std::shared_ptr<const LuauHostConfig> value) :
        config(std::move(value)) {
        if (config && !config->tape_json.empty()) {
            try {
                const auto options = Json::parse(config->tape_json);
                tape_mode = options.at("mode").get<std::string>();
                if (tape_mode == "replay") {
                    tape = options.at("entries");
                    if (!tape.is_array() || tape.size() > 4096) {
                        startup_error = "Invalid HTTP tape";
                    }
                } else if (tape_mode != "record") {
                    startup_error = "Invalid HTTP tape mode";
                }
            } catch (const std::exception&) {
                startup_error = "Invalid HTTP tape";
            }
        }
    }
    Connection connection(const std::string& name) {
        if (!config || !config->connections.contains(name)) {
            throw std::runtime_error("Unknown HTTP connection");
        }
        auto found = handles.find(name);
        if (found != handles.end()) {
            auto& value = connections.at(found->second);
            if (value.closed) {
                throw std::runtime_error("HTTP connection is closed");
            }
            return value.handle;
        }
        const auto& settings = config->connections.at(name);
        if (!settings.error.empty() && tape_mode != "replay") {
            throw std::runtime_error(settings.error);
        }
        Connection handle(std::make_shared<const int>(0));
        const auto* identity = handle.m_identity.get();
        connections.emplace(identity, ConnectionState {name, handle});
        handles.emplace(name, identity);
        return handle;
    }
    Status<std::string> close_result(Connection handle) {
        try {
            auto& impl = *this;
            const auto* pointer = handle.m_identity.get();
            auto found = impl.connections.find(pointer);
            if (found == impl.connections.end()) {
                return failure(std::string("Invalid HTTP connection handle"));
            }
            found->second.closed = true;
#ifndef __EMSCRIPTEN__
            for (const auto& [id, owner] : impl.requests) {
                if (owner == pointer) {
                    impl.pending.erase(id);
                    if (impl.client && !id.starts_with("replay:")) {
                        if (auto parsed = request_id(id)) {
                            impl.client->cancel(*parsed);
                        }
                    }
                }
            }
#endif
            return {};
        } catch (const std::exception& error) {
            return failure(std::string(error.what()));
        }
    }

#ifndef __EMSCRIPTEN__
    std::unique_ptr<ets::http::Client> client;
    Result<std::string, std::string> submit_result(Request options) {
        try {
            auto& impl = *this;
            if (!impl.startup_error.empty()) {
                return failure(impl.startup_error);
            }
            ets::http::Request request;
            request.url = std::move(options.url);
            const void* owner = nullptr;
            std::string path;
            if (options.connection) {
                owner = options.connection->m_identity.get();
                const auto found = impl.connections.find(owner);
                if (found == impl.connections.end()) {
                    return failure(
                        std::string("Invalid HTTP connection handle")
                    );
                }
                if (found->second.closed) {
                    return failure(std::string("HTTP connection is closed"));
                }
                if (!request.url.empty()) {
                    return failure(
                        std::string("Use path with a connection, not url")
                    );
                }
                path = std::move(options.path);
                if (!path.starts_with("/") || path.starts_with("//") ||
                    path.find_first_of("?#%\\\r\n") != std::string::npos ||
                    path.find("..") != std::string::npos ||
                    path.find('\0') != std::string::npos) {
                    return failure(
                        std::string(
                            "Expected a plain connection-relative HTTP path"
                        )
                    );
                }
                const auto& settings =
                    impl.config->connections.at(found->second.name);
                request.url = settings.base_url;
                while (request.url.ends_with("/")) {
                    request.url.pop_back();
                }
                request.url += path;
                request.headers = settings.headers;
            }
            request.method = std::move(options.method);
            request.body = std::move(options.body);
            auto seconds = &options.timeout;
            auto bytes = &options.max_response_bytes;
            if (*seconds <= 0 || *seconds > 3600 || *bytes < 1 ||
                *bytes > 64 * 1024 * 1024 || std::floor(*bytes) != *bytes) {
                return failure(std::string("Invalid HTTP limits"));
            }
            request.timeout = std::chrono::milliseconds(
                static_cast<long long>(std::ceil(*seconds * 1000))
            );
            request.max_response_bytes = static_cast<std::size_t>(*bytes);
            {
                if (options.headers.size() > 256) {
                    return failure(std::string("Too many HTTP headers"));
                }
                for (const auto& header : options.headers) {
                    std::string name = header.name;
                    const auto lower = [](std::string text) {
                        std::transform(
                            text.begin(),
                            text.end(),
                            text.begin(),
                            [](unsigned char c) {
                                return static_cast<char>(std::tolower(c));
                            }
                        );
                        return text;
                    };
                    const auto normalized = lower(name);
                    if (owner && (normalized == "authorization" ||
                                  normalized == "proxy-authorization" ||
                                  std::any_of(
                                      request.headers.begin(),
                                      request.headers.end(),
                                      [&](const auto& header) {
                                          return lower(header.first) ==
                                                 normalized;
                                      }
                                  ))) {
                        return failure(
                            std::string("Cannot override connection headers")
                        );
                    }
                    request.headers.emplace_back(std::move(name), header.value);
                }
            }
            Json fingerprint;
            if (owner && !impl.tape_mode.empty()) {
                fingerprint = {
                    {"connection", impl.connections.at(owner).name},
                    {"method", request.method},
                    {"path", path},
                    {"body", request.body}
                };
                if (impl.tape_mode == "replay") {
                    if (impl.tape_cursor >= impl.tape.size() ||
                        impl.tape[impl.tape_cursor].at("request") !=
                            fingerprint) {
                        return failure(
                            std::string("HTTP replay request mismatch")
                        );
                    }
                    auto id = "replay:" + std::to_string(impl.tape_cursor++);
                    impl.requests.emplace(id, owner);
                    impl.pending.insert(id);
                    return id;
                }
                impl.tape_bytes += fingerprint.dump().size();
                if (impl.tape.size() >= 4096 ||
                    impl.tape_bytes > 32ULL * 1024 * 1024) {
                    return failure(std::string("HTTP tape limit exceeded"));
                }
            }
            if (!impl.client) {
                auto client = ets::http::Client::create();
                if (!client) {
                    return failure(std::move(client.error()));
                }
                impl.client = std::move(*client);
            }
            auto submitted = impl.client->submit(std::move(request));
            if (!submitted) {
                return failure(std::move(submitted.error()));
            }
            auto id = std::to_string(*submitted);
            impl.pending.insert(id);
            if (owner) {
                impl.requests.emplace(id, owner);
            }
            if (!fingerprint.is_null()) {
                impl.recorded.emplace(id, impl.tape.size());
                impl.tape.push_back(
                    {{"request", fingerprint},
                     {"error", "Request cancelled or session closed"}}
                );
            }
            return id;
        } catch (const std::exception& error) {
            return failure(std::string(error.what()));
        }
    }
    Result<std::optional<Response>, std::string>
    poll_result(const std::string& id) {
        try {
            auto& impl = *this;
            const auto owned = impl.requests.find(id);
            if (owned != impl.requests.end() &&
                impl.connections.at(owned->second).closed) {
                impl.pending.erase(id);
                impl.requests.erase(owned);
                return failure(std::string("HTTP connection is closed"));
            }
            ets::http::Response response;
            if (std::string_view(id).starts_with("replay:")) {
                if (owned == impl.requests.end()) {
                    return failure(std::string("Unknown HTTP request"));
                }
                impl.pending.erase(id);
                impl.requests.erase(owned);
                auto index = request_id(std::string_view(id).substr(7));
                if (!index || *index >= impl.tape.size()) {
                    return failure(std::string("Unknown HTTP request"));
                }
                const auto& entry = impl.tape.at(*index);
                if (entry.contains("error")) {
                    return failure(
                        std::string(entry.at("error").get<std::string>())
                    );
                }
                const auto& value = entry.at("response");
                response.status = value.at("status").get<int>();
                response.body = value.at("body").get<std::string>();
                response.headers =
                    value.at("headers").get<ets::http::Headers>();
            } else {
                if (!impl.client) {
                    return failure(std::string("Unknown HTTP request"));
                }
                auto parsed = request_id(id);
                if (!parsed) {
                    return failure(std::move(parsed.error()));
                }
                auto result = impl.client->poll(*parsed);
                if (!result) {
                    return failure(std::move(result.error()));
                }
                if (!*result) {
                    return std::optional<Response> {};
                }
                auto completion = std::move(**result);
                impl.pending.erase(id);
                impl.requests.erase(id);
                const auto recording = impl.recorded.find(id);
                if (recording != impl.recorded.end()) {
                    auto& entry = impl.tape.at(recording->second);
                    if (completion) {
                        const auto& value = *completion;
                        entry["response"] = {
                            {"status", value.status},
                            {"headers", value.headers},
                            {"body", value.body}
                        };
                        entry.erase("error");
                    } else {
                        entry["error"] = completion.error();
                    }
                    impl.tape_bytes += entry.dump().size();
                    impl.recorded.erase(recording);
                    if (impl.tape_bytes > 32ULL * 1024 * 1024) {
                        return failure(std::string("HTTP tape limit exceeded"));
                    }
                }
                if (!completion) {
                    return failure(std::string(completion.error()));
                }
                response = std::move(*completion);
            }
            Response value {
                .status = response.status,
                .body = std::move(response.body)
            };
            for (auto& [name, text] : response.headers) {
                value.headers.push_back({std::move(name), std::move(text)});
            }
            return std::optional<Response> {std::move(value)};
        } catch (const std::exception& error) {
            return failure(std::string(error.what()));
        }
    }
    Status<std::string> cancel_result(const std::string& id) {
        try {
            auto& impl = *this;
            const bool pending = impl.pending.erase(id) != 0;
            impl.requests.erase(id);
            if (pending && impl.client &&
                !std::string_view(id).starts_with("replay:")) {
                if (auto parsed = request_id(id)) {
                    impl.client->cancel(*parsed);
                }
            }
            return {};
        } catch (const std::exception& error) {
            return failure(std::string(error.what()));
        }
    }
#else
    Result<std::string, std::string> submit_result(Request) {
        return failure(
            std::string("HTTP is not supported by the browser host yet")
        );
    }
    Result<std::optional<Response>, std::string>
    poll_result(const std::string&) {
        return failure(
            std::string("HTTP is not supported by the browser host yet")
        );
    }
    Status<std::string> cancel_result(const std::string&) {
        return failure(
            std::string("HTTP is not supported by the browser host yet")
        );
    }
#endif
};
Library::Library(LuauLibraryServices& services) :
    m_impl(std::make_unique<Impl>(services.config)) {}
std::string Library::tape_json() const {
    if (m_impl->tape_mode.empty()) {
        return {};
    }
    return Impl::Json {
        {"version", 1},
        {"mode", m_impl->tape_mode},
        {"consumed", m_impl->tape_cursor},
        {"entries", m_impl->tape}
    }.dump();
}
Library::~Library() = default;
void Library::cancel() noexcept {
    m_impl->pending.clear();
    m_impl->requests.clear();
#ifndef __EMSCRIPTEN__
    if (m_impl->client) {
        m_impl->client->cancel_all();
    }
#endif
}
std::size_t Library::pending() const {
    return m_impl->pending.size();
}
std::string Library::diagnostic(std::string_view key) const {
    return key == "tape" ? tape_json() : std::string {};
}
Connection Library::connection(std::string name) {
    return m_impl->connection(name);
}
void Library::close(Connection connection) {
    auto result = m_impl->close_result(connection);
    if (!result) {
        throw std::runtime_error(result.error());
    }
}
std::string Library::submit(Request request) {
    auto result = m_impl->submit_result(std::move(request));
    if (!result) {
        throw std::runtime_error(result.error());
    }
    return std::move(*result);
}
std::optional<Response> Library::poll(std::string id) {
    auto result = m_impl->poll_result(id);
    if (!result) {
        throw std::runtime_error(result.error());
    }
    return std::move(*result);
}
void Library::cancel_request(std::string id) {
    auto result = m_impl->cancel_result(id);
    if (!result) {
        throw std::runtime_error(result.error());
    }
}
} // namespace ets::luau::http
