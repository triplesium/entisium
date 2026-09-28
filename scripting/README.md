# Scripting

Start here to write Luau scripts, use the script libraries, or extend Entisium's
script API.

## Libraries and guides

| Module | Source | Guide |
| --- | --- | --- |
| `@task` | [Task](libraries/task/task.luau) | [Tasks and scheduling](docs/luau-task.md) |
| `@schema` | [Schema](libraries/schema/schema.luau) | [Schemas and validation](docs/luau-schema.md) |
| `@json` | [JSON native adapter](libraries/json/json.hpp) | [JSON](docs/luau-json.md) |
| `@http` | [HTTP](libraries/http/http.luau) | [HTTP](docs/luau-http.md) |
| `@ai` | [AI](libraries/ai/ai.luau) | [AI and host configuration](docs/luau-ai.md) |
| `@context/core` | [Context](libraries/context/context.luau) | [Context](docs/luau-context.md) |
| `@context` | [Playtest context adapter](libraries/playtest/context.luau) | [Context](docs/luau-context.md) |
| `@playtest/game` | [Game control](libraries/playtest/game.luau) | [Playtest](docs/playtest.md) |
| `@playtest/scheduler` | [Playtest scheduler](libraries/playtest/scheduler.luau) | [Playtest scheduling](docs/playtest.md#scheduling) |

For runnable projects, see [Examples](examples/README.md). For AI-driven tests,
see [Decisions](docs/playtest-decisions.md) and [LLM generation](docs/playtest-llm.md).
Importing an asynchronous library does not start a task session; Playtest supplies
the scheduler described in the Task and Playtest guides.

## Extending the script API

Each library keeps its Luau source, native C++ adapter, xmake target and tests
together under `libraries/`. See [Library authoring](docs/luau-libraries.md) for
native, pure Luau and mixed libraries, generated bindings, and SDK publication.
Playtest modules use the same library rule. The standalone host links the
`entisium-luau-playtest` bundle; game hosts select it only in their SDK targets
so the Editor can type-check tests without exposing Playtest in the game VM.
See [Host SDK builds](docs/luau-libraries.md#host-sdk-builds) for independent
SDK targets and output paths.

## Implementation boundaries

- [engine/scripting](../engine/scripting/): VM, compiler, reflection bridge,
  module loading, ECS integration and execution pools.
- [scripting/libraries](libraries/): script-facing APIs and their native adapters.
  Native HTTP transport remains in [engine/http](../engine/http/).
- [runtime/playtest](../runtime/playtest/): standalone Luau process entrypoint.
- [devkit/src/luau](../devkit/src/luau/) and
  [devkit/src/playtest](../devkit/src/playtest/): process management,
  configuration and game/test orchestration.
- [tools/luau_libraries](../tools/luau_libraries/) and
  [tools/luau_defgen](../tools/luau_defgen/): library catalogs, bindings and SDK
  generation from reflection and the selected host libraries.
- [tools/entisium_lsp](../tools/entisium_lsp/): native and WASM language service,
  consumed by the Editor.

The public module names and xmake target names are independent of this source
layout. `entisium-scripting-core` supplies the engine implementation;
`entisium-scripting` bundles the usual libraries, and hosts may select individual
`entisium-luau-*` targets instead.
