# Ordinary LLM generation

`@ai` supports stateless text and structured generation alongside Jev decisions.

The shared implementation and public types live in `scripting/libraries/ai/ai.luau`.
It uses native `@http` connection handles and `@json`. Jev, Responses and Chat
Completions protocols run in Luau; DevKit resolves configuration and credentials
at process startup. Game and Playtest VMs load the same built-in libraries.
See [AI and host configuration](luau-ai.md) for the binding lifecycle.

Generation runs in a child task which owns the HTTP request. Cancelling the task
cancels its pending request. The response is validated with `@schema` before it
is returned to the caller. Luau receives no authorization header or API key.

## Configuration

Merge these fields into the existing `.entisium/config.yaml` selected for your
game project, or set `ETS_CONFIG_PATH` to a shared configuration file:

```yaml
version: 1
providers:
  main:
    type: openai
    apiKey: YOUR_API_KEY
    chat:
      api: responses
llm:
  models:
    planner:
      provider: main
      id: YOUR_MODEL_ID
  timeoutMs: 30000
  maxTokens: 4096
```

Choose a model available to your account. There is no implicit default LLM model.
An OpenAI-compatible provider can set `type: openai-compatible` and
`chat: { api: chat-completions, baseUrl: https://your-provider/v1 }` (or Responses
when supported). OpenRouter uses its existing provider defaults. Structured
generation requires JSON Schema structured-output support on the selected
endpoint/model; unsupported requests fail without switching protocols or silently
falling back to text prompting.

Credential precedence: provider `apiKey`, injected
`runPlaytest({ resolveCredential })`, then `OPENAI_API_KEY` for type `openai` or
`OPENROUTER_API_KEY` for type `openrouter`. Custom compatible endpoints require an
explicit key or resolver. The existing project/user config selection is unchanged.

## Luau API

```lua
local ai = require("@ai")
local schema = require("@schema")
local task = require("@task")

local planner = ai.model {
    model = "planner",
    instructions = "Choose the next goal from the supplied game state.",
    output = schema.object {
        action = schema.enum { "attack", "retreat", "collect" },
        target = schema.optional(schema.integer()),
        reason = schema.string():describe("Brief reason for the choice"),
    },
}
local result = task.timeout(planner:generate {
    context = view:sample(),
    prompt = "Choose the next goal.",
}, 20):await()

controller:set_goal(result.data.action, result.data.target)
```

`ai.model` captures the model alias, optional instructions and optional output
schema. `generate` requires `prompt`; `context` is optional and accepts a context
sample with `encode()`. Context is serialized as JSON in a separate user message,
not appended to system instructions. Data, field descriptions and frame metadata
are retained. Each call has no hidden history, tools, automatic actions or agent
loop. The caller remains responsible for executing and checking chosen goals.

The result contains `data`, `model` and
`usage: { input_tokens, output_tokens }`. `model` uses the provider-reported model
from the response. Without `output`, `data` is a string.
With `output`, it is strictly validated against the schema, and Luau types infer
the output fields through `:await()`. Enum static types remain strings.

The returned task supports shared waiting, cancellation, `race` and `all` through
[`@task`](luau-task.md). Timeout and race completion detach waits; explicit
cancellation or owner-scope exit cancels underlying work. Waiting uses
the existing paused-simulation scheduling policy. Configuration `timeoutMs` and
`task.timeout` use milliseconds and seconds respectively; the whole Luau run still
has its own default 60-second limit (`runPlaytest.timeoutMs` can override it).

## Structured output semantics

`ai.luau` adapts `@schema` definitions to strict Responses `text.format` or
Chat Completions `response_format`. `@schema` validates the decoded result.

The wire response is wrapped as `{ "data": ... }`, allowing primitive schemas
while preserving the root-object requirement. Optional fields become required
nullable fields on the wire, then null is normalized to an absent field. Null is
never accepted for required fields. Optional array elements cannot become holes.
The wrapper is removed before returning `result.data` to Luau. Descriptions are
preserved. No coercion, default insertion, unknown-field stripping or JSON repair
is performed. Validation errors identify the field path.

Explicit refusal, truncation, invalid JSON, invalid values, empty output, network
failure, timeout and cancellation are errors catchable with Luau `pcall`. Calls
are not retried. Provider error bodies are not exposed. Requests have no tools;
unexpected tool calls are errors. Complete response bodies are bounded to 2 MiB, prompt and
instructions to 64 KiB each, and context to 1 MiB. These are byte limits.
Requests set `stream=false`; streaming is not implemented.
Backend schema restrictions beyond this supported subset remain provider errors.

Protocol reference: [OpenAI structured outputs](https://developers.openai.com/api/docs/guides/structured-outputs).

## Run and replay

The example requests four movement decisions and a final text summary:

```powershell
$env:ETS_CONFIG_PATH = "$PWD/.entisium/config.yaml"
$env:ETS_RUNTIME_HOST_PATH = "$PWD/build/windows/x64/release/entisium-runtime-host.exe"
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/llm.luau --record llm-run.json
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/llm.luau --replay llm-run.json
```

Records use the native HTTP tape format described in [Luau AI](luau-ai.md).
They preserve request/response order without credentials. Replay requires matching
model/connection aliases and protocol metadata; it does not perform provider calls.
Old model-service tapes are not supported. Timing and wall-clock races are not
reproduced. Use `modelTape` for programmatic recording and replay.

Regression coverage uses real loopback HTTP from native Luau and the native game
runtime. This does not establish live-model quality or endpoint-specific support.
