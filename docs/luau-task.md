# Tasks and host scheduling

`@task` is the shared cooperative asynchronous module. Its implementation is
[`engine/scripting/libraries/task/task.luau`](../engine/scripting/libraries/task/task.luau); it imports no Playtest, AI, ECS or Node module. The
standalone host exposes the alias and generated SDK types. Game-runtime module
loading is not yet connected to it.

```lua
local task = require("@task")

local worker = task.spawn(function()
    task.on_cleanup(function() release_resources() end)
    local pending = planner:generate { context = view:sample(), prompt = "Choose" }
    return task.timeout(pending, 20):await().data
end)

local decision = worker:await()
```

## Public operations

| Operation | Result |
| --- | --- |
| `task.spawn(fn)` | Schedule a coroutine; return `Task<T>` for its first return value |
| `pending:await()` | Suspend the current managed coroutine, return cached result or throw |
| `pending:status()` | `pending`, `succeeded`, `failed` or `cancelled` |
| `pending:cancel()` | Request cancellation of this task and its owned children |
| `task.all { name = pending, ... }` | Task containing named results once all succeed |
| `task.race { name = pending, ... }` | Task containing `{ kind = name, value = result }` for the first settlement; failure/cancellation propagates |
| `task.sleep(seconds)` | Task completing after elapsed wall time |
| `task.timeout(pending, seconds)` | Wrapper failing with `Wall timeout` when its deadline wins |
| `task.on_cleanup(fn)` | Register a callback on the current coroutine, in reverse execution order |
| `task.completion(onCancel?)` | Completion source `{ task, resolve, reject }` for event adapters |

Creation starts operations/timers immediately. Spawn schedules work for the
scheduler rather than running it inline in the creator. Durations are finite,
nonnegative seconds. Real timers have scheduler/host wakeup granularity, not
hard real-time precision.

Types are exported by the implementation, for example `task.Task<number>` and
`task.TaskHost`. Named `all` inputs preserve each field's result type. Named
`race` inputs produce a discriminated union: checking `winner.kind` narrows
`winner.value` to the corresponding result type.

Each task settles once and retains its result. Multiple coroutines may await it,
including after completion. Results are shared, not cloned per waiter. `all`
retains named results (nil values are absent Lua table fields); empty groups are
rejected. `race` compares settlement timestamps and then settlement order, not
branch names. Task creation and waiting require a running task session. Awaiting
your own task is an error. Handles cannot be used in another session.

## Ownership and cancellation

Every operation created by a coroutine belongs to that coroutine's scope.
Normal completion, failure or cancellation closes the scope and cancels unfinished
children recursively. A parent that needs a child's result must await it before
returning; returning an unfinished child handle does not transfer ownership.

Awaiting an existing task does not adopt it. Cancelling a waiter, cancelling a
combination, losing a race, or timing out detaches that wait without cancelling
its inputs. Explicit `pending:cancel()` still cancels the underlying operation
for every waiter. An owner exiting still cancels its children even if another
coroutine is waiting for them. To keep work alive, create it in a longer-lived
owner scope.

```lua
local pending = planner:generate(input)
local winner = task.race {
    model = pending,
    deadline = task.sleep(2),
}:await()
if winner.kind == "deadline" then
    -- Explicit policy: this caller owns the operation and no longer needs it.
    pending:cancel()
end
```

Cancellation is processed by the scheduler; callbacks run once before that task's
waiters resume. A task cancelling itself does not continue its body. Cancellation
of external operations invokes the adapter hook (Node aborts the associated
request). Completed operations are not cancelled again. Unobserved failed child
tasks fail their owner when its scope exits; awaited/composed failures can be
handled with `pcall`.

Cleanup runs after descendant cleanup, once, in reverse registration order. It
may make host cleanup calls such as releasing input, but cannot spawn tasks,
await, or register further cleanup. Cleanup errors fail the task. Pending
coroutines are closed; arbitrary code after their last yield does not execute.

## Event adapters

```lua
local source = task.completion(function()
    disconnect_listener()
end) :: task.TaskCompletion<number>

-- From a coroutine or the host update callback:
source.resolve(42)
-- Or: source.reject("operation failed")

local value: number = source.task:await()
```

Resolve/reject enqueue settlement and do not run waiters inline. Repeated or
late settlement after cancellation/session disposal is ignored. Host adapters may
pass a second argument: the settlement timestamp in the host's monotonic
millisecond clock. Public event sources otherwise use the scheduler's current
time. Because completion creation has no value from which to infer a type, use
the explicit `task.TaskCompletion<T>` cast for typed sources. This is a completion
primitive, not a full ECS signal/subscription API.

## Host contract

Hosts start a root scope using `task.run(entry, adapter)`; nested sessions are
rejected. The adapter supplies:

- `now()`: monotonic time in milliseconds.
- `update()`: optional event delivery, resolving/rejecting completion sources.
- `idle({ timers })`: wait/pump the host; return false when nothing can progress.
- `handle_yield(event)`: optional bridge for host-specific coroutine yields.

The core drains settlements before resuming ready coroutines. It never advances
a game or chooses whether simulation should pause. Host setup/transport failure
also closes the root scope and attempts cleanup. Each session is bounded to 4096
task objects; the Node host additionally limits external submissions to 2048.

The Playtest adapter is `runtime/playtest/luau/scheduler.luau`. It advances one
shared simulation clock only when no coroutine can progress and a game wait is
pending. Pending external requests pause simulation; other ready coroutines can
still run. Pure wall waits do not advance the game. Idle wall/external waits use
bounded 25 ms host waits. A future real-time host can pump game frames while
requests remain pending by supplying a different adapter.

Service submission is owned by libraries, not `@task`. The AI library uses native
`@http` connection handles and `task.poll` to deliver results and cancellation.
Playtest pauses simulation while native HTTP is pending; Node still supplies its
clock and game-control event loop.

## Game waits

```lua
game.next_step():await()
game.delay(1):await() -- simulation seconds
task.sleep(1):await() -- wall seconds
```

Game waits return ordinary Tasks. Their deadlines/step targets start at creation.

Race losers and timed-out inputs remain active. Cancel them explicitly, or put
operations in a spawned owner scope that ends when the race completes. Under
Playtest's pause policy, a pending race loser can keep simulation paused until
it finishes or is cancelled.

## Frame-driven sessions and polling sources

`task.start(entry, adapter?)` returns a frozen handle with `step()` and `close()`.
Call `step()` once per game update; it runs ready work until idle and returns whether
the session has finished. The default adapter uses the native monotonic clock.
Only one session may be active per VM. A session starts on its first step.
Closing cancels children and runs cleanup. Errors propagate from step/close.

`task.poll(read, onCancel?)` creates a typed task from a nonblocking polling source.
`read()` returns `(ready, value)` and may throw to reject the task. Pending sources
keep the host event loop alive without allocating a timer each iteration. A read
callback must not block or yield. `@http` uses this primitive for native completions.
See [Luau HTTP](luau-http.md) for game and Playtest examples.
