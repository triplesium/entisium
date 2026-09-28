# Decision services and asynchronous tasks

For ordinary text and structured LLM generation, see [LLM generation](LLM.md).
Both use the native HTTP tape format described in [Luau AI](../../docs/luau-ai.md).

Playtest submits model decisions from Luau. DevKit resolves configuration and
credentials; native HTTP workers send requests. The shared Luau implementation
implements TypeSafe's Jev `POST /v1/systemone` contract:
https://docs.typesafe.ai/api

The bundled native example is
`samples/projects/playtest_basics/assets/tests/decision.luau`. It samples a context,
asks which direction to move, applies one step of input, and checks its objective.
It also asserts that awaiting the model does not advance simulation time.

## Configuration and running

Use the existing Entisium config selection (`ETS_CONFIG_PATH`, the project's
`.entisium/config.yaml`, or the user config). Add an optional decision model map:

```yaml
version: 1
providers:
  typesafe:
    type: typesafe
    apiKey: YOUR_TYPESAFE_KEY
    decisions:
      baseUrl: https://api.typesafe.ai/v1
decisions:
  models:
    fast-decision:
      provider: typesafe
      id: jev-latest
  timeoutMs: 30000
```

Without a decision configuration, `fast-decision` defaults to TypeSafe's
`jev-latest`, and reads `TYPESAFE_API_KEY`. Explicit provider keys take precedence.
Provider names are arbitrary: `type: typesafe` selects the default TypeSafe endpoint
and `TYPESAFE_API_KEY` fallback even under a custom name. Every configured provider
must declare its `type`; provider names do not determine it.
Applications can inject `DecisionService` or `resolveCredential(provider, signal)`
into `runPlaytest` to reuse their existing credential store. DevKit imports no
Agent or Editor implementation. Providers of other types must explicitly configure a
TypeSafe-compatible `decisions.baseUrl`; chat completion endpoints are not
interchangeable with this protocol.

Build `entisium-runtime-host` and `entisium-luau-host` as described in README.md.
Set `ETS_RUNTIME_HOST_PATH` when using a release build, then run from the repo root:

```powershell
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/decision.luau --record decisions.json
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/decision.luau --replay decisions.json
```

Recording writes a new file and refuses to overwrite an existing file. It records
context data and descriptions, questions, requested model alias, returned model
version, results/errors and elapsed wall time. Do not record confidential context
into a file you intend to publish. Provider credentials are not part of the tape.
Replay needs no credentials and makes no provider calls. It checks each request
against canonical state, questions and model alias, rejects mismatches and rejects
unused records after a successful test. Network delays are not reproduced: this
is decision replay, not replay of wall-clock races or scheduling.

## Luau API

```luau
local ai = require("@ai")
local task = require("@task")

local policy = ai.decision {
    model = "fast-decision",
    questions = {
        movement = ai.choice {
            instructions = "Choose the next direction using data.player.x",
            criteria = { right = "Increase x", stay = "Hold position" },
        },
        danger = ai.noul { instructions = "Is the player in immediate danger?" },
        progress = ai.score {
            instructions = "Rate progress toward the exit",
            criteria = { "At the start", "Halfway", "At the exit" },
        },
    },
}

local result = task.timeout(policy:evaluate(view:sample()), 2):await()
local movement = result.answers.movement
if movement.type == "choice" then
    print(movement.choice, movement.confidence)
end
```

Context JSON is parsed back into a structured `state` object, retaining schema
descriptions and sample timing metadata. Questions contain their own instructions;
their IDs are used to look up answers. This initial wrapper supports string
instructions and descriptions, 2–255 Choice options, 2–10 Score levels, and Noul.
The result preserves the provider's resolved model, answers, probabilities,
confidence, score legend and optional usage. Noul has a probability and no separate
confidence field. Invalid answer types, option sets, distributions and score
ranges fail explicitly. There are no automatic retries of paid requests.

## Native model requests

The shared `@ai` library implements Jev with `@http` and `@json`. DevKit resolves
YAML aliases and injects model metadata and native-only credentials at startup.
Game and Playtest VMs use the same bindings. Engine HTTP workers send requests
independently of Node task dispatch. See [AI configuration](../../docs/luau-ai.md).
Tasks support `await()`, `status()`, `cancel()`, `task.all` and `task.race`.
Explicit cancellation and owner-scope exit cancel pending HTTP operations.

`task.timeout(pending, seconds)` uses elapsed real-world seconds. For simulation
deadlines, race against `game.delay(seconds)`. A completed task's settlement timestamp is
checked against its wall deadline, so polling cannot admit a late result. The
provider also has its own request timeout, and the entire runner retains its
wall-clock timeout and hard-kill fallback.

Playtest pauses simulation while any native HTTP request or submitted host task is pending.
Other ready Luau coroutines can continue reading snapshots or changing inputs.
The scheduler uses bounded 25 ms host waits when idle, rather than occupying the
protocol for the full model request. A session is limited to 2048 submissions.
Real-time simulation during inference and image inputs are not included yet.
Ordinary LLM generation is described in [LLM generation](LLM.md).

## Validation

The regression runs the example against a local HTTP server, records four
decisions, then replays against a fresh game process with an unreachable endpoint.
This validates integration and replay, not actual model decision quality.
Configure a TypeSafe key to run the example against the hosted model.
