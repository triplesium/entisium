# Luau playtests

Tests run in a separate Luau process. Node owns the test and native game processes,
and forwards host requests. The game retains its own ECS and simulation.
Asynchronous tasks and native Jev model requests are described in
[Decision services](DECISIONS.md), including configuration and recording/replay.
Ordinary text and schema-constrained generation are documented in [LLM generation](LLM.md).

## Run the example

From the repository root, build `entisium-runtime-host` and `entisium-luau-host`
with xmake, then run:

```powershell
xmake build -y entisium-runtime-host entisium-luau-host
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/movement.luau
```

The default native executable is the development/debug build. For a release build:

```powershell
$env:ETS_RUNTIME_HOST_PATH = "$PWD/build/windows/x64/release/entisium-runtime-host.exe"
```

The Luau executable is found beside it; `ETS_LUAU_HOST_PATH` overrides that path.
The entry must be inside the project's asset directory. Tests can use relative
imports or `project://` imports within that directory.

## Write a test

```luau
local task = require("@task")
local game = require("@playtest/game")
local Gameplay = require("../main")

return function()
    game.start()
    task.on_cleanup(game.input.release_all)
    game.input.press("D")
    local reached = game.when(function()
        return game:read_resource(Gameplay.PlayerState).x >= 4
    end)
    local result = (task.race { reached = reached, timeout = game.delay(2) }):await()
    assert(result.kind == "reached", "Player did not reach the target")
    game.input.release("D")
end
```

An entry can instead export `run`. Importing a game module makes its reflected
types available in the test VM; it does not install its Plugins or create a World.
Module top-level code still executes, as with ordinary `require`.

`read_resource(Type)` returns an owned, read-only snapshot. `query(Type, ...)`
returns rows with an entity identifier and a `components` array in requested type
order. Snapshots use the existing reflection serializer and are restored as typed
values in the test VM. Reading does not mark ECS resources as changed. Old
snapshots remain unchanged when the game advances. Unsupported serialization or
type mismatches fail explicitly. Type names and field schemas are checked across
processes; numeric IDs are never used to identify types across VMs.

The generated SDK publishes the actual `--!strict` Luau library sources, including
`@playtest/game` and `@task`, alongside C++ native declarations. Resource
reads and single-component queries infer the imported type; queries with multiple
different component types currently return `any` component values. Runtime
read-only enforcement does not make Luau's static type `T` immutable.

## Scheduling

The shared scheduler is now [`@task`](../../docs/luau-task.md). Game waits and AI requests
return `Task<T>` objects with `:await()`, `:cancel()` and `:status()`.

- `game.delay(seconds):await()` waits in simulation time.
- `game.when(predicate):await()` polls a condition at
  simulation boundaries; predicates may read snapshots, but must not call await.
- `game.next_step():await()` provides fixed-step feedback for controllers.
- `task.spawn(function)` returns a cooperative task. Awaited failures can be caught;
  unobserved failures fail the owner on scope exit. Scope exit cancels pending children.
- `(task.all { name = wait, ... }):await()` waits for all branches.
- `(task.race { name = wait, ... }):await()` returns `{ kind = winningName, value = ... }`.
  Settlement time/order determines the winner; losers are not cancelled automatically.
- `task.on_cleanup(function)` registers task cleanup in reverse registration order.
  Remaining child cleanup runs before root cleanup when the test ends.

There is one shared simulation clock. Snapshot reads and input changes do not
advance it. When no coroutine is ready, a game wait is pending, and there is no
pending external request, the adapter advances one fixed step and checks conditions.
Pure wall waits do not advance the game. Parallel waits therefore do not each
advance the game. Key state persists until changed or released. `game.capture()`
returns the native frame capture, including its image attachment.

The runner has a default 60-second wall-clock limit, separate from simulated wait
timeouts. SIGINT/SIGTERM request cancellation, allowing input cleanup before a
bounded process-kill fallback. VM instruction budgets interrupt runaway scripts.
The runner always stops the game process. This is not a security sandbox for
untrusted scripts.

## Implementation boundaries

- `engine/scripting`: reusable source resolution, cached module loading, type
  preparation, resumable entry functions and JSON host-call replies.
- `devkit/src/luau/host.ts`: process transport, dispatch, cancellation and limits.
- `runtime/playtest`: filesystem host adapter and cooperative test SDK.
- `devkit/src/playtest`: project/runtime orchestration and CLI.
- Native providers `test.snapshot`, `test.input`, `test.advance`: ECS reads,
  persistent keyboard state and explicit simulation advancement.

Playtest supports native execution and keyboard input. It retains existing playtest
segments. Browser execution, mouse/gamepad APIs, hot reload, editor/MCP test-run
commands are not part of this version. Snapshot queries are
bounded to 32 requested types, 4096 entities and 1 MiB of serialized output.
