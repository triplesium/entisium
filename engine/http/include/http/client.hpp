#pragma once
#include "base/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ets::http {
using Headers = std::vector<std::pair<std::string, std::string>>;
struct Request {
    std::string method {"GET"};
    std::string url;
    Headers headers;
    std::string body;
    std::chrono::milliseconds timeout {30000};
    std::size_t max_response_bytes {8 * 1024 * 1024};
};
struct Response {
    int status {};
    Headers headers;
    std::string body;
};
using Completion = Result<Response, std::string>;
// Thread-safe, bounded HTTP client. A successful poll with an empty optional
// means the request is still pending. Poll consumes completed results;
// unknown or consumed IDs return a Result error. Cancellation is idempotent.
class Client {
  public:
    static Result<std::unique_ptr<Client>, std::string> create(
        std::size_t concurrency = 4,
        std::size_t capacity = 64,
        std::string ca_file = {}
    );
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Result<std::uint64_t, std::string> submit(Request request);
    Result<std::optional<Completion>, std::string> poll(std::uint64_t id);
    void cancel(std::uint64_t id);
    void cancel_all();

  private:
    Client(std::size_t concurrency, std::size_t capacity, std::string ca_file);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace ets::http
