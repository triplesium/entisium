#pragma once
#include "scripting/annotations.hpp" // IWYU pragma: keep
#include "scripting/library.hpp"

#include <optional>
namespace ets {
template<class T>
class LuauLibraryBinding;
}
namespace ets::luau::http {
ETS_REFLECT()
class Connection {
  public:
    Connection(const Connection&) = default;
    Connection& operator=(const Connection&) = default;
    bool operator==(const Connection&) const = default;

  private:
    friend class Library;
    explicit Connection(std::shared_ptr<const void> identity) :
        m_identity(std::move(identity)) {}
    std::shared_ptr<const void> m_identity;
};
ETS_REFLECT()
struct Header {
    std::string name, value;
};
ETS_REFLECT(LuauType(
    input =
        "{read url: string, read method: string?, read headers: {Header}?, "
        "read body: string?, read timeout: number?, read max_response_bytes: "
        "number?} | {read connection: Connection, read path: string, read "
        "method: string?, read headers: {Header}?, read body: string?, read "
        "timeout: number?, read max_response_bytes: number?}"
))
struct Request {
    std::string url, path;
    std::optional<Connection> connection;
    std::string method {"GET"}, body;
    double timeout {30};
    double max_response_bytes {8 * 1024 * 1024};
    std::vector<Header> headers;
};
ETS_REFLECT()
struct Response {
    int status;
    std::vector<Header> headers;
    std::string body;
};
ETS_REFLECT(LuauLibrary(name = "@http"))
class Library final {
  public:
    explicit Library(LuauLibraryServices& services);
    ~Library();
    Connection connection(std::string name);
    void close(Connection connection);
    std::string submit(Request request);
    std::optional<Response> poll(std::string id);
    ETS_REFLECT(LuauExport(name = "cancel"))
    void cancel_request(std::string id);
    std::size_t pending() const;

  private:
    friend class ets::LuauLibraryBinding<Library>;
    void cancel() noexcept;
    std::string diagnostic(std::string_view key) const;
    std::string tape_json() const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace ets::luau::http
