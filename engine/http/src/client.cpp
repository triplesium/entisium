#include "http/client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ets::http {
namespace {
using Clock = std::chrono::steady_clock;
bool token(const std::string& text) {
    return !text.empty() && std::ranges::all_of(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                   std::string_view::npos;
    });
}
Result<std::pair<std::string, std::string>, std::string>
split_url(const std::string& url) {
    const auto start = url.starts_with("https://") ? 8U :
                       url.starts_with("http://")  ? 7U :
                                                     0U;
    if (start == 0 || url.size() > 16384 ||
        std::ranges::any_of(url, [](unsigned char c) {
            return c <= 32 || c == 127;
        })) {
        return failure(
            std::string("Expected an HTTP(S) URL without whitespace")
        );
    }
    const auto end = url.find_first_of("/?#", start);
    const auto authority = url.substr(start, end - start);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        url.find('#') != std::string::npos) {
        return failure(
            std::string("Invalid URL authority, credentials or fragment")
        );
    }
    auto path = end == std::string::npos ? "/" : url.substr(end);
    if (path.front() == '?') {
        path.insert(path.begin(), '/');
    }
    return std::pair {url.substr(0, end), std::move(path)};
}
} // namespace
struct Client::Impl {
    using Clock = std::chrono::steady_clock;
    struct Job {
        Request request;
        Clock::time_point deadline;
        std::atomic_bool cancelled {false};
        std::optional<Completion> result;
        Response response;
        CURL* easy {};
        curl_slist* headers {};
        bool oversized {};
        std::size_t header_bytes {};
        ~Job() {
            if (easy) {
                curl_easy_cleanup(easy);
            }
            curl_slist_free_all(headers);
        }
    };
    std::mutex mutex;
    bool closing {};
    std::size_t capacity;
    std::string ca_file;
    std::uint64_t next {1};
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    CURLM* multi {};
    std::jthread worker;

    explicit Impl(std::size_t limit, std::string certificate_file) :
        capacity(limit), ca_file(std::move(certificate_file)) {}
    Status<std::string> start(std::size_t concurrency) {
        struct Global {
            CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
            ~Global() {
                if (status == CURLE_OK) {
                    curl_global_cleanup();
                }
            }
        };
        static const Global global;
        if (global.status != CURLE_OK) {
            return failure(std::string(curl_easy_strerror(global.status)));
        }
        const auto* version = curl_version_info(CURLVERSION_NOW);
        if (!(version->features & CURL_VERSION_ASYNCHDNS)) {
            return failure(
                std::string("HTTP requires libcurl asynchronous DNS")
            );
        }
        multi = curl_multi_init();
        if (!multi) {
            return failure(std::string("Cannot create HTTP multi handle"));
        }
        worker = std::jthread([this, concurrency] {
            work(concurrency);
        });
        return {};
    }
    ~Impl() {
        {
            std::scoped_lock lock(mutex);
            closing = true;
        }
        cancel_all();
        if (worker.joinable()) {
            worker.join();
        }
        if (multi) {
            curl_multi_cleanup(multi);
        }
    }
    void wake() const {
        if (multi) {
            (void)curl_multi_wakeup(multi);
        }
    }
    void cancel_all() {
        {
            std::scoped_lock lock(mutex);
            for (auto& [id, job] : jobs) {
                (void)id;
                job->cancelled = true;
            }
            jobs.clear();
            queue.clear();
        }
        wake();
    }
    static std::size_t write_body(
        char* data,
        std::size_t size,
        std::size_t count,
        void* context
    ) noexcept {
        auto& job = *static_cast<Job*>(context);
        const auto bytes = size * count;
        if (job.cancelled) {
            return 0;
        }
        if (bytes > job.request.max_response_bytes - job.response.body.size()) {
            job.oversized = true;
            return 0;
        }
        try {
            job.response.body.append(data, bytes);
        } catch (...) {
            return 0;
        }
        return bytes;
    }
    static std::size_t write_header(
        char* data,
        std::size_t size,
        std::size_t count,
        void* context
    ) noexcept {
        auto& job = *static_cast<Job*>(context);
        const auto bytes = size * count;
        if (job.cancelled || bytes > 65536 - job.header_bytes) {
            return 0;
        }
        job.header_bytes += bytes;
        try {
            std::string_view line(data, bytes);
            if (line.starts_with("HTTP/")) {
                job.response.headers.clear();
            } else if (
                const auto colon = line.find(':');
                colon != std::string_view::npos
            ) {
                auto value = line.substr(colon + 1);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() &&
                       (value.back() == '\r' || value.back() == '\n' ||
                        value.back() == ' ' || value.back() == '\t')) {
                    value.remove_suffix(1);
                }
                job.response.headers.emplace_back(line.substr(0, colon), value);
            }
        } catch (...) {
            return 0;
        }
        return bytes;
    }
    template<typename T>
    static void option(CURL* easy, CURLoption key, T value) {
        const auto code = curl_easy_setopt(easy, key, value);
        if (code != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(code));
        }
    }
    void prepare(Job& job) const {
        job.easy = curl_easy_init();
        if (!job.easy) {
            throw std::runtime_error("Cannot create HTTP request");
        }
        auto* easy = job.easy;
        option(easy, CURLOPT_URL, job.request.url.c_str());
        option(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        // Preserve the direct-connection behavior of the previous transport.
        option(easy, CURLOPT_PROXY, "");
        option(easy, CURLOPT_NOSIGNAL, 1L);
        option(easy, CURLOPT_FOLLOWLOCATION, 0L);
        option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!ca_file.empty()) {
            option(easy, CURLOPT_CAINFO, ca_file.c_str());
        }
        const auto remaining =
            (std::max)(1L,
                       static_cast<long>(std::chrono::duration_cast<
                                             std::chrono::milliseconds>(
                                             job.deadline - Clock::now()
                       )
                                             .count()));
        option(easy, CURLOPT_TIMEOUT_MS, remaining);
        option(easy, CURLOPT_CONNECTTIMEOUT_MS, (std::min)(remaining, 1000L));
        if (!job.request.body.empty() || job.request.method == "POST" ||
            job.request.method == "PUT" || job.request.method == "PATCH") {
            option(easy, CURLOPT_POSTFIELDS, job.request.body.data());
            option(
                easy,
                CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(job.request.body.size())
            );
        }
        option(easy, CURLOPT_CUSTOMREQUEST, job.request.method.c_str());
        if (job.request.method == "HEAD") {
            option(easy, CURLOPT_NOBODY, 1L);
        }
        for (const auto& [key, value] : job.request.headers) {
            auto line = key;
            line += value.empty() ? ";" : ": ";
            line += value;
            auto* headers = curl_slist_append(job.headers, line.c_str());
            if (!headers) {
                throw std::bad_alloc();
            }
            job.headers = headers;
        }
        option(easy, CURLOPT_HTTPHEADER, job.headers);
        option(easy, CURLOPT_WRITEFUNCTION, &write_body);
        option(easy, CURLOPT_WRITEDATA, &job);
        option(easy, CURLOPT_HEADERFUNCTION, &write_header);
        option(easy, CURLOPT_HEADERDATA, &job);
    }
    static void check(CURLMcode code) {
        if (code != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(code));
        }
    }
    void publish(const std::shared_ptr<Job>& job, Completion result) {
        std::scoped_lock lock(mutex);
        if (!job->cancelled && !job->result) {
            job->result = std::move(result);
        }
    }
    void work(std::size_t concurrency) noexcept {
        // Only this thread operates on transfers attached to the multi handle.
        std::unordered_map<CURL*, std::shared_ptr<Job>> active;
        try {
            while (true) {
                int wait_ms = 1000;
                {
                    std::scoped_lock lock(mutex);
                    if (closing) {
                        break;
                    }
                    for (auto& [id, job] : jobs) {
                        (void)id;
                        if (job->result) {
                            continue;
                        }
                        const auto remaining = std::chrono::duration_cast<
                                                   std::chrono::milliseconds>(
                                                   job->deadline - Clock::now()
                        )
                                                   .count();
                        if (remaining <= 0) {
                            job->cancelled = true;
                            job->result =
                                failure(std::string("HTTP request timed out"));
                        } else {
                            wait_ms = (std::min)(wait_ms,
                                                 static_cast<int>(remaining));
                        }
                    }
                    std::erase_if(queue, [](const auto& job) {
                        return job->cancelled.load();
                    });
                }
                for (auto it = active.begin(); it != active.end();) {
                    if (it->second->cancelled) {
                        check(curl_multi_remove_handle(multi, it->first));
                        it = active.erase(it);
                    } else {
                        ++it;
                    }
                }
                while (active.size() < concurrency) {
                    std::shared_ptr<Job> job;
                    {
                        std::scoped_lock lock(mutex);
                        if (queue.empty() || closing) {
                            break;
                        }
                        job = std::move(queue.front());
                        queue.pop_front();
                    }
                    if (job->cancelled) {
                        continue;
                    }
                    try {
                        prepare(*job);
                        active.emplace(job->easy, job);
                        const auto code =
                            curl_multi_add_handle(multi, job->easy);
                        if (code != CURLM_OK) {
                            active.erase(job->easy);
                            check(code);
                        }
                    } catch (const std::exception& error) {
                        publish(job, failure(std::string(error.what())));
                    }
                }
                int running = 0;
                check(curl_multi_perform(multi, &running));
                int pending = 0;
                while (auto* message = curl_multi_info_read(multi, &pending)) {
                    if (message->msg != CURLMSG_DONE) {
                        continue;
                    }
                    const auto found = active.find(message->easy_handle);
                    if (found == active.end()) {
                        continue;
                    }
                    auto job = found->second;
                    const auto code = message->data.result;
                    long status = 0;
                    (void)curl_easy_getinfo(
                        job->easy,
                        CURLINFO_RESPONSE_CODE,
                        &status
                    );
                    check(curl_multi_remove_handle(multi, job->easy));
                    active.erase(found);
                    if (Clock::now() >= job->deadline ||
                        code == CURLE_OPERATION_TIMEDOUT) {
                        publish(
                            job,
                            failure(std::string("HTTP request timed out"))
                        );
                    } else if (job->oversized) {
                        publish(
                            job,
                            failure(
                                std::string("HTTP response exceeds size limit")
                            )
                        );
                    } else if (code != CURLE_OK) {
                        publish(
                            job,
                            failure(
                                "HTTP transport: " +
                                std::string(curl_easy_strerror(code))
                            )
                        );
                    } else {
                        job->response.status = static_cast<int>(status);
                        publish(job, std::move(job->response));
                    }
                }
                {
                    std::scoped_lock lock(mutex);
                    if (closing ||
                        (!queue.empty() && active.size() < concurrency)) {
                        continue;
                    }
                }
                check(curl_multi_poll(multi, nullptr, 0, wait_ms, nullptr));
            }
        } catch (const std::exception& error) {
            std::scoped_lock lock(mutex);
            closing = true;
            for (auto& [id, job] : jobs) {
                (void)id;
                if (!job->result) {
                    job->result = failure(std::string(error.what()));
                }
            }
        }
        // Independent transfers can be detached in any order during shutdown.
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order)
        for (const auto& [easy, job] : active) {
            (void)job;
            (void)curl_multi_remove_handle(multi, easy);
        }
    }
};
Client::Client(
    std::size_t concurrency,
    std::size_t capacity,
    std::string ca_file
) : m_impl(std::make_unique<Impl>(capacity, std::move(ca_file))) {}
Client::~Client() = default;
Result<std::unique_ptr<Client>, std::string> Client::create(
    std::size_t concurrency,
    std::size_t capacity,
    std::string ca_file
) {
    if (concurrency == 0 || concurrency > 32 || capacity == 0 ||
        capacity > 1024) {
        return failure(std::string("Invalid HTTP client limits"));
    }
    try {
        auto client = std::unique_ptr<Client>(
            new Client(concurrency, capacity, std::move(ca_file))
        );
        if (auto started = client->m_impl->start(concurrency); !started) {
            return failure(std::move(started.error()));
        }
        return client;
    } catch (const std::exception& error) {
        return failure(std::string(error.what()));
    }
}
Result<std::uint64_t, std::string> Client::submit(Request request) {
    if (auto endpoint = split_url(request.url); !endpoint) {
        return failure(std::move(endpoint.error()));
    }
    if (!token(request.method) || request.timeout.count() <= 0 ||
        request.timeout > std::chrono::hours(1) ||
        request.max_response_bytes == 0 ||
        request.max_response_bytes > 64ULL * 1024 * 1024 ||
        request.body.size() > 8ULL * 1024 * 1024) {
        return failure(std::string("Invalid HTTP request or limits"));
    }
    std::size_t header_size = 0;
    for (const auto& [key, value] : request.headers) {
        header_size += key.size() + value.size();
        if (!token(key) || value.find_first_of("\r\n") != std::string::npos ||
            value.find('\0') != std::string::npos || header_size > 65536) {
            return failure(std::string("Invalid HTTP headers"));
        }
        std::string lower = key;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower == "content-length" || lower == "transfer-encoding" ||
            lower == "host") {
            return failure(
                std::string("HTTP framing headers are managed by the client")
            );
        }
    }
    auto job = std::make_shared<Impl::Job>();
    job->deadline = Clock::now() + request.timeout;
    job->request = std::move(request);
    std::scoped_lock lock(m_impl->mutex);
    if (m_impl->closing || m_impl->jobs.size() >= m_impl->capacity ||
        m_impl->queue.size() >= m_impl->capacity) {
        return failure(std::string("HTTP request capacity exceeded"));
    }
    const auto id = m_impl->next++;
    m_impl->jobs.emplace(id, job);
    m_impl->queue.push_back(std::move(job));
    m_impl->wake();
    return id;
}
Result<std::optional<Completion>, std::string> Client::poll(std::uint64_t id) {
    std::scoped_lock lock(m_impl->mutex);
    const auto found = m_impl->jobs.find(id);
    if (found == m_impl->jobs.end()) {
        return failure(std::string("Unknown HTTP request"));
    }
    if (!found->second->result) {
        return std::optional<Completion> {};
    }
    auto result = std::move(found->second->result);
    m_impl->jobs.erase(found);
    return result;
}
void Client::cancel(std::uint64_t id) {
    {
        std::scoped_lock lock(m_impl->mutex);
        const auto found = m_impl->jobs.find(id);
        if (found == m_impl->jobs.end()) {
            return;
        }
        found->second->cancelled = true;
        std::erase(m_impl->queue, found->second);
        m_impl->jobs.erase(found);
    }
    m_impl->wake();
}
void Client::cancel_all() {
    m_impl->cancel_all();
}
} // namespace ets::http
