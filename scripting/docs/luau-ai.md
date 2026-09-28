# Luau AI and host configuration

`@ai`, `@schema`, `@json`, `@http` and `@task` are built-in modules shared by game
and Playtest VMs. Public AI types and protocols live in `scripting/libraries/ai/ai.luau`.
Importing modules does not register services or require configured providers.

## Configuration

DevKit reads the existing project/user `.entisium/config.yaml` selection, including
`ETS_CONFIG_PATH`. For example:

```yaml
version: 1
providers:
  fast:
    type: typesafe
  text:
    type: openai
    chat:
      api: responses
decisions:
  models:
    fast-decision: { provider: fast, id: jev-latest }
  timeoutMs: 30000
llm:
  models:
    planner: { provider: text, id: YOUR_MODEL_ID }
  timeoutMs: 30000
  maxTokens: 4096
```

Credentials resolve in this order: provider `apiKey`, the supplied credential
resolver, then the provider-type environment variable (`TYPESAFE_API_KEY`,
`OPENAI_API_KEY`, `OPENROUTER_API_KEY`). Configuration diagnostics do not echo keys.
Custom providers can use `type: openai-compatible` with `chat.baseUrl` and
`chat.api: responses | chat-completions`, or `decisions.baseUrl` for Jev.
No default ordinary LLM alias is invented.

## Startup and lifetime

1. `configuredLuauHost` resolves aliases, model IDs, protocols, limits and connections.
2. DevKit writes a versioned, bounded envelope to the process's private inherited
   stdin pipe using `--config-stdin`. It is not placed in command-line arguments,
   temporary files, logs or public Luau tables.
3. The game scripting plugin passes immutable configuration to its primary VM and
   every execution-pool lane. The standalone host uses the same parser and VM
   constructor. Connection handles and requests are local to each VM.
4. `ai.model` / `ai.decision` resolves the alias to metadata and an opaque handle.
   Missing configuration, unknown aliases, missing credentials and closed connections
   fail explicitly when used.
5. HTTP workers attach credentials and send requests. The Luau scheduler consumes
   completions. Cancellation ignores late results; VM destruction cancels outstanding
   work and releases connections.

Configuration is a startup snapshot. Restart to apply edits or refresh credentials;
live reconfiguration is not implemented. Embedders can pass `LuauHostConfig` to
`LuauRuntime`, `LuauExecutionPool` or `LuauScriptingPlugin`. `NativeRuntime` and
`runPlaytest` share the DevKit resolver. Running a native binary directly without
`--config-stdin` leaves model configuration unavailable.

## Luau usage

Inside an existing task session (Playtest already provides one):

```luau
local ai = require("@ai")
local schema = require("@schema")
local planner = ai.model {
    model = "planner",
    instructions = "Choose the next action.",
    output = schema.object {
        action = schema.enum {"move", "wait"},
        reason = schema.string(),
        target = schema.optional(schema.integer()),
    },
}
local result = planner:generate {
    prompt = "What should happen next?", context = view:sample(),
}:await()
```

`result.data` is validated and typed; `result.model` and `result.usage` retain metadata.
`ai.decision` retains the choice, score and noul APIs in the
[Jev guide](playtest-decisions.md). Ordinary generation supports complete
Responses and Chat Completions responses; see [generation semantics](playtest-llm.md).
For game scripts, construct and use models inside a `task.start` entry and step
the session from the frame loop, following the [HTTP guide](luau-http.md).
Tasks, connections and requests are transient host state, not ECS snapshot data.

Requests have no hidden history, tools, retries or automatic actions. Context is
sent in a separate user message. Structured output uses strict JSON Schema,
normalizes nullable optional fields at the protocol boundary and validates with
`@schema`. Refusals, truncated output, unexpected tool calls, invalid data, HTTP
errors, timeouts and cancellation are errors. HTTP error bodies are not exposed.

## Recording and replay

Playtest's `--record` / `--replay` flags now record connection-scoped native HTTP.
The format is `entisium.http-tape`, version 1; old model-service tapes are not
supported. Fingerprints include connection name, method, path and body; authorization
headers and base URLs are excluded. Responses and transport errors are recorded
in submission order. Replay checks requests and unused entries without network calls.

Files contain prompts, observations and model output. Each tape is limited to 4096
requests and 32 MiB, written exclusively with owner-only permissions where supported.
A hard-killed host produces an incomplete tape, which cannot be replayed. Native game
requests use the same configuration but are not part of the Playtest script's tape.

## Validation

Real loopback HTTP tests exercise Jev metadata and invalid answers, both ordinary
LLM protocols, text and structured output, optional nulls, cancellation, deadlines,
connection closure and credential isolation. Controller regressions read YAML in
both hosts, record runs, and replay with an unreachable endpoint. These tests make
no paid provider calls.
