# Luau HTTP

`@http` sends requests in the native process using `engine/http` and libcurl multi.
Node and DevKit do not proxy the request. Both native game VMs and the Playtest
Luau host expose the same built-in module. Browser hosts reject requests with an
explicit unsupported error.

```luau
local http = require("@http")

-- Inside a running Task (for example a Playtest suite's run function):
local response = http.request({
    method = "POST",
    url = "https://example.com/api",
    headers = {{ name = "Content-Type", value = "application/json" }},
    body = '{"message":"hello"}',
    timeout = 15,
    max_response_bytes = 1024 * 1024,
}):await()
assert(response.status == 200)
print(response.body)
```

## Contract

Types are defined in `scripting/libraries/http/http.luau` and copied to the generated SDK.
`request(options)` returns `task.Task<http.Response>`. Cancellation propagates to
the native request; scope cancellation discards late results. Calls outside a Task
session fail before submitting network work.

- `method`: optional, defaults to `GET`.
- `url`: absolute HTTP/HTTPS URL. Userinfo, fragments and whitespace are rejected.
- `headers`: optional array of `{name, value}` entries, preserving duplicates.
  Host, Content-Length and Transfer-Encoding are managed by the transport.
- `body`: optional string, including binary bytes; maximum 8 MiB.
- `timeout`: positive wall seconds, including queueing, default 30, maximum 3600.
  Each connection attempt is additionally limited to one second, bounding native
  shutdown during DNS/connect operations.
- `max_response_bytes`: positive integer, default 8 MiB, maximum 64 MiB.

Responses contain `status`, `headers` and `body`. Response tables and header entries
are frozen. HTTP 4xx/5xx return normally. Transport failure, timeout and oversized
responses reject the Task. No redirects or application retries are automatic.
JSON encoding/decoding belongs to the caller; the HTTP module treats bodies as bytes.

HTTPS verifies the certificate chain and hostname using system trust. The C++
client can accept an explicit CA file; the Luau API cannot disable verification.

## Game frame integration

A game can drive a session without blocking an ECS update:

```luau
local task = require("@task")
local http = require("@http")

local requests = task.start(function()
    local response = http.request({url = "https://example.com"}):await()
    assert(response.status == 200)
end)

local function update()
    requests.step() -- true after completion; propagates failures
end

export local NetworkPlugin = Plugin.new {
    build = function(app: App)
        app:add_system(Update, update)
    end,
}
```

`task.start` starts lazily on the first `step`. Call `close()` when abandoning a
session, including when its owning feature is removed. There is one active Task
session per VM; multiple requests should be children of that session. Do not keep
borrowed ECS references across an await. Network requests and coroutine sessions
are transient host state and are not included in ECS snapshots or rollback.

Playtest already owns its Task session; call `http.request` directly there.
HTTP does not advance simulation. Playtest's scheduler chooses when game waits
advance the game, while network requests run concurrently in the native process.

## Implementation and limits

The native `ets::http::Client` returns `Result` for creation and submission.
`poll(id)` returns `Result<std::optional<Completion>, std::string>`: an empty
optional means pending, `Completion` is `Result<Response, std::string>` for a
finished transfer, and an outer error means the request ID is unknown or has
already been consumed. `cancel(id)` is idempotent. The Luau binding converts
these errors to script errors at the callback boundary.

Each VM lazily creates a client with one network thread, up to four concurrent
transfers and capacity for 64 unconsumed requests/results. Submit and cancel
update bounded queues/flags and wake the network thread. That thread owns the
libcurl multi handle and removes cancelled transfers. It never touches Lua
values; the script thread consumes completions when the Task scheduler runs.
Deadlines include time spent queued. Shutdown cancels queued/active work and
joins the network thread. The dependency enables c-ares for asynchronous DNS.
TLS peer and hostname verification remain enabled, redirects remain disabled,
and proxy environment variables are not used.

Streaming and the browser HTTP backend are outside this version.
Model configuration and protocols are documented in [Luau AI](luau-ai.md).

## Tests

`entisium-http-tests` uses local HTTP/TLS servers for bodies, duplicate headers,
status codes, response limits, queue deadlines, cancellation, shutdown, certificate
trust and hostname verification. Certificates in `engine/http/tests/fixtures` are
public test credentials only, and are never installed into system trust.
`entisium-luau-libraries-tests` exercises the same bindings without Node;
`entisium-lsp-tests` checks source types and response inference.

## Configured connections

Model bindings contain an opaque `http.Connection` owned by their VM. Internally,
`@ai` calls `http.request({connection = handle, path = "/responses", ...})`.
Connection requests cannot also supply `url`. Paths must start with a single `/`,
with no query, fragment, percent encoding, backslash or `..` segments. A connection
cannot target another origin. Redirect following remains disabled.

Authorization and other connection headers are attached by C++ and cannot be read
or overridden in Luau. `http.close(handle)` permanently closes the connection in
that VM and cancels pending requests. Models sharing that connection are affected.
Further binding or requests report a closed-connection error. VM destruction
cancels requests and releases connections. A new process loads fresh configuration.
Direct URL requests remain available independently.

Playtest waits for native HTTP before advancing simulation; other ready coroutines
can still run. Game VMs follow their normal frame loop.
