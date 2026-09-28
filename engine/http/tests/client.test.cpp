#include "http/client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <thread>
using namespace std::chrono_literals;
namespace {
struct Server {
    httplib::Server server;
    std::jthread thread;
    std::string url;
    std::atomic_bool entered {false};
    std::atomic_bool release {false};
    Server() {
        server.Post(
            "/echo",
            [](const httplib::Request& request, httplib::Response& response) {
                response.status = 422;
                response.set_header("X-Duplicate", "a");
                response.set_header("X-Duplicate", "b");
                response.set_content(request.body, "application/octet-stream");
            }
        );
        server.Get(
            "/large",
            [](const httplib::Request&, httplib::Response& response) {
                response.set_content(std::string(65536, 'x'), "text/plain");
            }
        );
        server.Get(
            "/redirect",
            [](const httplib::Request&, httplib::Response& response) {
                response.set_redirect("/large", 302);
            }
        );
        server.Get(
            "/wait",
            [this](const httplib::Request&, httplib::Response& response) {
                entered = true;
                while (!release) {
                    std::this_thread::sleep_for(1ms);
                }
                response.set_content("done", "text/plain");
            }
        );
        const auto port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        url = "http://127.0.0.1:" + std::to_string(port);
        thread = std::jthread([this] {
            server.listen_after_bind();
        });
        server.wait_until_ready();
    }
    ~Server() {
        release = true;
        server.stop();
        thread.join();
    }
    void wait_entered() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!entered && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(entered);
    }
};
ets::http::Completion wait(ets::http::Client& client, std::uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto polled = client.poll(id);
        REQUIRE(polled);
        if (*polled) {
            return std::move(**polled);
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL("HTTP completion did not arrive");
    return {};
}
} // namespace
TEST_CASE(
    "HTTP preserves binary bodies, duplicate headers and error statuses",
    "[http]"
) {
    Server server;
    auto client = std::move(*ets::http::Client::create());
    const std::string body("a\0b", 3);
    const auto id = *client->submit(
        {.method = "POST", .url = server.url + "/echo", .body = body}
    );
    auto result = wait(*client, id);
    if (!result) {
        INFO(result.error());
    }
    REQUIRE(result);
    if (!result) {
        return;
    }
    CHECK(result->status == 422);
    CHECK(result->body == body);
    std::size_t duplicates = 0;
    for (const auto& [name, value] : result->headers) {
        (void)value;
        if (name == "X-Duplicate") {
            ++duplicates;
        }
    }
    CHECK(duplicates == 2);
    auto consumed = client->poll(id);
    REQUIRE_FALSE(consumed);
    CHECK(consumed.error() == "Unknown HTTP request");
    const auto oversized = wait(
        *client,
        *client->submit(
            {.url = server.url + "/large", .max_response_bytes = 32}
        )
    );
    CHECK_FALSE(oversized);
    CHECK(oversized.error().find("size limit") != std::string::npos);
}
TEST_CASE("HTTP deadlines include queueing and capacity is bounded", "[http]") {
    Server server;
    auto client = std::move(*ets::http::Client::create(1, 2));
    const auto active = *client->submit({.url = server.url + "/wait"});
    server.wait_entered();
    const auto queued =
        *client->submit({.url = server.url + "/large", .timeout = 25ms});
    auto full = client->submit({.url = server.url + "/large"});
    REQUIRE_FALSE(full);
    CHECK(full.error() == "HTTP request capacity exceeded");
    const auto timeout = wait(*client, queued);
    CHECK_FALSE(timeout);
    CHECK(timeout.error() == "HTTP request timed out");
    client->cancel(active);
    CHECK_FALSE(client->poll(active));
    server.release = true;
    auto result =
        wait(*client, *client->submit({.url = server.url + "/large"}));
    REQUIRE(result);
}
TEST_CASE(
    "HTTP cancels active transfers and discards late responses",
    "[http]"
) {
    Server server;
    auto client = std::move(*ets::http::Client::create());
    const auto id =
        *client->submit({.url = server.url + "/wait", .timeout = 50ms});
    server.wait_entered();
    CHECK(wait(*client, id).error() == "HTTP request timed out");
    client->cancel(id);
    CHECK_FALSE(client->poll(id));
    server.release = true;
}
TEST_CASE("HTTP shutdown cancels active requests", "[http]") {
    Server server;
    const auto start = std::chrono::steady_clock::now();
    {
        auto client = std::move(*ets::http::Client::create());
        (void)client->submit({.url = server.url + "/wait"});
        server.wait_entered();
    }
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}
TEST_CASE(
    "HTTP cancellation releases concurrency before the server responds",
    "[http]"
) {
    Server server;
    auto client = std::move(*ets::http::Client::create(1, 4));
    const auto active = *client->submit({.url = server.url + "/wait"});
    server.wait_entered();
    const auto queued =
        *client->submit({.url = server.url + "/large", .timeout = 1500ms});
    client->cancel(active);
    CHECK_FALSE(client->poll(active));
    REQUIRE(wait(*client, queued));
    CHECK_FALSE(server.release);
}
TEST_CASE(
    "HTTP transfers progress concurrently and cancel all remains reusable",
    "[http]"
) {
    Server server;
    auto client = std::move(*ets::http::Client::create(2, 4));
    const auto active = *client->submit({.url = server.url + "/wait"});
    server.wait_entered();
    REQUIRE(wait(
        *client,
        *client->submit({.url = server.url + "/large", .timeout = 1500ms})
    ));
    client->cancel_all();
    CHECK_FALSE(client->poll(active));
    REQUIRE(wait(*client, *client->submit({.url = server.url + "/large"})));
}
TEST_CASE("HTTP preserves HEAD and does not follow redirects", "[http]") {
    Server server;
    auto client = std::move(*ets::http::Client::create());
    auto head = wait(
        *client,
        *client->submit({.method = "HEAD", .url = server.url + "/large"})
    );
    REQUIRE(head);
    CHECK(head->status == 200);
    CHECK(head->body.empty());
    auto redirect =
        wait(*client, *client->submit({.url = server.url + "/redirect"}));
    REQUIRE(redirect);
    CHECK(redirect->status == 302);
    CHECK(redirect->body.empty());
}
TEST_CASE("HTTP rejects invalid URLs, framing headers and limits", "[http]") {
    auto invalid_client = ets::http::Client::create(0, 1);
    REQUIRE_FALSE(invalid_client);
    CHECK(invalid_client.error() == "Invalid HTTP client limits");
    CHECK_FALSE(ets::http::Client::create(1, 0));
    auto client = std::move(*ets::http::Client::create());
    auto invalid_url = client->submit({.url = "file:///tmp/file"});
    REQUIRE_FALSE(invalid_url);
    CHECK(invalid_url.error() == "Expected an HTTP(S) URL without whitespace");
    CHECK_FALSE(client->submit({.url = "http://user:password@localhost/"}));
    CHECK_FALSE(client->submit(
        {.url = "http://localhost/", .headers = {{"X-Test", "a\r\nb"}}}
    ));
    CHECK_FALSE(client->submit(
        {.url = "http://localhost/", .headers = {{"Content-Length", "100"}}}
    ));
    CHECK_FALSE(client->submit({.url = "http://localhost/", .timeout = 0ms}));
}

TEST_CASE("HTTPS validates trust and hostname", "[http][tls]") {
    const std::string certificate =
        std::string(ETS_HTTP_FIXTURES) + "/localhost.pem";
    const std::string key =
        std::string(ETS_HTTP_FIXTURES) + "/localhost-key.pem";
    httplib::SSLServer server(certificate.c_str(), key.c_str());
    REQUIRE(server.is_valid());
    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("secure", "text/plain");
    });
    const auto port = server.bind_to_any_port("0.0.0.0");
    REQUIRE(port > 0);
    std::jthread thread([&] {
        server.listen_after_bind();
    });
    struct Stop {
        httplib::Server& server;
        ~Stop() { server.stop(); }
    } stop {server};
    server.wait_until_ready();
    const auto url = "https://127.0.0.1:" + std::to_string(port) + "/";
    auto untrusted = std::move(*ets::http::Client::create());
    CHECK_FALSE(wait(*untrusted, *untrusted->submit({.url = url})));
    auto trusted = std::move(*ets::http::Client::create(2, 8, certificate));
    const auto good = wait(*trusted, *trusted->submit({.url = url}));
    if (!good) {
        INFO(good.error());
    }
    REQUIRE(good);
    if (!good) {
        return;
    }
    CHECK(good->body == "secure");
    const auto wrong_host = "https://127.0.0.2:" + std::to_string(port) + "/";
    CHECK_FALSE(wait(*trusted, *trusted->submit({.url = wrong_host})));
}
