--!strict
local native = require("@internal/task")
export type Task<T> = {
    read __task_value: T,
    read await: (Task<T>) -> T,
    read cancel: (Task<T>) -> (),
    read status: (Task<T>) -> ("pending" | "succeeded" | "failed" | "cancelled"),
}
export type TaskCompletion<T> = {
    read task: Task<T>, read resolve: (T, number?) -> (), read reject: (string, number?) -> (),
}
export type TaskHost = {
    now: () -> number,
    update: (() -> ())?,
    idle: (({ timers: boolean }) -> boolean)?,
    handle_yield: ((any) -> any)?,
}
-- Cooperative tasks. The host supplies clocks, event delivery and idle policy.
local api = {}
export type Status = "pending" | "succeeded" | "failed" | "cancelled"
type Outcome = { status: Status, value: any, failure: string?, at: number, sequence: number }
type Node = { kind: string, session: Session, status: Status, children: {Node}, cleanups: {() -> ()},
    owner: Node?, handle: Task<any>, terminal: Outcome?, observed: boolean?, thread: thread?,
    onCancel: (() -> ())?, deadline: number?, target: Node?, inputs: {[string]: Node}?, names: {string}?,
    poll: (() -> (boolean, any))?, waiting: Node?, value: any, failure: string?, at: number, sequence: number }
type Session = { nodes: {Node}, now: number, sequence: number, adapter: TaskHost }
local handles: {[any]: Node} = setmetatable({}, { __mode = "k" })
local methods = {}
local state: Session? = nil
local current: Node? = nil

local function session(): Session
    assert(state, "Operation requires a task session")
    return state
end

local function managed(): Node
    assert(state and current and not current.terminal, "Operation requires a running task")
    return current
end
local function get(handle: Task<any>): Node
    local node = handles[handle]
    assert(node, "Expected a Task")
    assert(node.session == state, "Task belongs to another session")
    return node
end
local function duration(seconds: number): ()
    assert(type(seconds) == "number" and seconds >= 0 and seconds < math.huge, "Invalid duration")
end
local function clock(): number
    local state = session()
    state.now = state.adapter.now()
    return state.now
end
local function create(kind: string): Node
    local state = session()
    assert(#state.nodes < 4096, "Task session limit exceeded")
    local handle = table.freeze(setmetatable({}, table.freeze { __index = methods }))
    -- Heterogeneous nodes erase only the result type; typed constructors restore it.
    local node: Node = { kind = kind, session = state, status = "pending", children = {}, cleanups = {}, owner = current,
        handle = handle :: any, value = nil, at = 0, sequence = 0 }
    handles[handle] = node
    table.insert(state.nodes, node)
    if current then table.insert(current.children, node) end
    return node
end
local function terminate(node: Node, status: Status, value: any?, failure: string?, settled: number?): ()
    local state = session()
    if node.status ~= "pending" or node.terminal then return end
    state.sequence += 1
    node.terminal = { status = status, value = value, failure = failure, at = settled or state.now, sequence = state.sequence }
    for _, child in node.children do terminate(child, "cancelled", nil, "Task cancelled") end
end
local function result(node: Node): any
    if node.status ~= "succeeded" then error(node.failure or "Task cancelled", 3) end
    return node.value
end
function methods.await<T>(self: Task<T>): T
    managed()
    local node = get(self)
    assert(node ~= current, "A task cannot await itself")
    node.observed = true
    if node.status ~= "pending" then return result(node) end
    coroutine.yield({ kind = "task.await", node = node })
    return result(node)
end
function methods.status<T>(self: Task<T>): Status
    local node = handles[self]
    assert(node, "Expected a Task")
    return if node.terminal then node.terminal.status else node.status
end
function methods.cancel<T>(self: Task<T>): ()
    local node = get(self)
    terminate(node, "cancelled", nil, "Task cancelled")
    if node == current then coroutine.yield({ kind = "task.cancel" }) end
end

function api.spawn<T>(fn: () -> T): Task<T>
    managed()
    assert(type(fn) == "function", "spawn expects a function")
    local node = create("coroutine")
    node.thread = coroutine.create(fn)
    return node.handle
end
function api.on_cleanup(fn: () -> ()): ()
    local current = managed()
    assert(type(fn) == "function", "on_cleanup expects a function")
    table.insert(current.cleanups, fn)
end
function api.completion<T>(onCancel: (() -> ())?): TaskCompletion<T>
    managed()
    assert(onCancel == nil or type(onCancel) == "function", "Expected cancellation callback")
    local node = create("completion")
    node.onCancel = onCancel
    return table.freeze {
        task = node.handle,
        resolve = function(value: T, settled: number?)
            if state == node.session then terminate(node, "succeeded", value, nil, settled) end
        end,
        reject = function(failure: string, settled: number?)
            if state == node.session then terminate(node, "failed", nil, tostring(failure), settled) end
        end,
    }
end
-- Polling sources are checked once per host update, without allocating timer tasks.
function api.poll<T>(read: () -> (boolean, T?), onCancel: (() -> ())?): Task<T>
    managed()
    local node = create("poll")
    node.poll = read
    node.onCancel = onCancel
    return node.handle
end
function api.sleep(seconds: number): Task<nil>
    managed(); duration(seconds)
    local deadline = clock() + seconds * 1000
    local node = create("sleep")
    node.deadline = deadline
    return node.handle
end
function api.timeout<T>(handle: Task<T>, seconds: number): Task<T>
    managed(); duration(seconds)
    local target = get(handle)
    target.observed = true
    local deadline = clock() + seconds * 1000
    local node = create("timeout")
    node.target, node.deadline = target, deadline
    return node.handle
end
local function group(kind: string, children: {[string]: Task<any>}): Task<any>
    managed()
    assert(type(children) == "table", "Expected named tasks")
    local inputs: {[string]: Node} = {}
    local names: {string} = {}
    for name, handle in children do
        assert(type(name) == "string", "Task names must be strings")
        local child = get(handle)
        inputs[name] = child
        table.insert(names, name)
    end
    assert(#names > 0, "Task group must not be empty")
    table.sort(names)
    local node = create(kind)
    node.inputs, node.names = inputs, names
    for _, child in inputs do child.observed = true end
    return node.handle
end
export type function TaskValues(children, race)
    local result = types.newtable()
    local variants = {}
    for key, property in children:properties() do
        local handle = property.read or property.write
        local value = handle:readproperty(types.singleton("__task_value"))
        if not value then error("Expected named Task values") end
        if race:value() then
            local variant = types.newtable()
            variant:setreadproperty(types.singleton("kind"), key)
            variant:setreadproperty(types.singleton("value"), value)
            table.insert(variants, variant)
        else result:setreadproperty(key, value) end
    end
    if race:value() then
        if #variants == 0 then error("Expected a nonempty task group") end
        return if #variants == 1 then variants[1] else types.unionof(unpack(variants))
    end
    return result
end
function api.all<T>(children: T): Task<TaskValues<T, false>> return group("all", children :: any) end
function api.race<T>(children: T): Task<TaskValues<T, true>> return group("race", children :: any) end

local function finalize(): boolean
    local state = session()
    local changed = false
    for i = #state.nodes, 1, -1 do
        local node = state.nodes[i]
        if node.status ~= "pending" or not node.terminal then continue end
        local outcome = node.terminal
        local errors = {}
        if outcome.status == "cancelled" and node.onCancel then
            local ok, failure = pcall(node.onCancel)
            if not ok then table.insert(errors, tostring(failure)) end
        end
        if node.thread and coroutine.status(node.thread) ~= "dead" then
            local ok, failure = pcall(coroutine.close, node.thread)
            if not ok then table.insert(errors, tostring(failure)) end
        end
        for j = #node.cleanups, 1, -1 do
            local ok, failure = pcall(node.cleanups[j])
            if not ok then table.insert(errors, tostring(failure)) end
        end
        table.clear(node.cleanups)
        for _, child in node.children do
            if child.status == "failed" and not child.observed then table.insert(errors, child.failure or "Task failed") end
        end
        node.status, node.value, node.failure = outcome.status, outcome.value, outcome.failure
        node.at, node.sequence = outcome.at, outcome.sequence
        node.terminal = nil
        if #errors > 0 then
            node.status = "failed"
            node.failure = (node.failure and node.failure .. "\n" or "") .. table.concat(errors, "\n")
        end
        changed = true
    end
    return changed
end
local function propagate(node: Node, target: Node): ()
    terminate(node, target.status, target.value, target.failure, target.at)
end
local function update(): ()
    local state = session()
    for _, node in state.nodes do
        if node.status ~= "pending" or node.terminal then continue end
        if node.kind == "poll" then
            assert(node.poll)
            local ok, ready, value = pcall(node.poll)
            if not ok then terminate(node, "failed", nil, tostring(ready))
            elseif ready then terminate(node, "succeeded", value) end
        elseif node.kind == "sleep" then
            assert(node.deadline)
            if state.now >= node.deadline then terminate(node, "succeeded", nil, nil, node.deadline) end
        elseif node.kind == "timeout" then
            assert(node.target and node.deadline)
            if node.target.status ~= "pending" and node.target.at <= node.deadline then propagate(node, node.target)
            elseif state.now >= node.deadline then terminate(node, "failed", nil, "Wall timeout", node.deadline) end
        elseif node.kind == "all" or node.kind == "race" then
            assert(node.names and node.inputs)
            local ready, values = true, {}
            local winner: Node? = nil
            local name: string? = nil
            for _, key in node.names do
                local child = node.inputs[key]
                if child.status == "pending" then ready = false
                else
                    values[key] = child.value
                    if not winner or child.at < winner.at or (child.at == winner.at and child.sequence < winner.sequence) then
                        winner, name = child, key
                    end
                    if node.kind == "all" and child.status ~= "succeeded" then propagate(node, child); break end
                end
            end
            if node.kind == "race" and winner then
                if winner.status == "succeeded" then terminate(node, "succeeded", table.freeze { kind = name, value = winner.value }, nil, winner.at)
                else propagate(node, winner) end
            elseif node.kind == "all" and ready then terminate(node, "succeeded", table.freeze(values)) end
        end
    end
end
local function drive(node: Node): ()
    local state = session()
    assert(node.thread)
    node.waiting = nil
    local reply: any = nil
    while not node.terminal do
        current = node
        local ok, event = coroutine.resume(node.thread, reply)
        current = nil
        if not ok then terminate(node, "failed", nil, debug.traceback(node.thread, tostring(event))); break end
        if coroutine.status(node.thread) == "dead" then terminate(node, "succeeded", event); break end
        if type(event) == "table" and event.kind == "task.await" then node.waiting = event.node; break
        elseif type(event) == "table" and event.kind == "task.cancel" then break
        else
            assert(state.adapter.handle_yield, "Unmanaged coroutine yield")
            reply = state.adapter.handle_yield(event)
        end
    end
end

local function drain(): boolean
    local progressed = false
    while true do
        update()
        if not finalize() then return progressed end
        progressed = true
    end
end

-- Hosts own the event loop: update resolves completions; idle waits or advances simulation.
function api.run<T>(entry: () -> T, adapter: TaskHost): T
    assert(not state, "Nested task sessions are unsupported")
    assert(type(entry) == "function", "Expected task entry")
    local active: Session = { nodes = {}, now = 0, sequence = 0, adapter = adapter }
    state = active
    local updateHost = adapter.update or function() end
    local idleHost = adapter.idle or function() return false end
    local root = create("coroutine")
    root.thread = coroutine.create(entry)
    local ok, failure = pcall(function()
        while root.status == "pending" do
            clock()
            updateHost()
            local progressed = drain()
            for _, node in active.nodes do
                if node.kind == "coroutine" and node.status == "pending" and not node.terminal and
                    (not node.waiting or node.waiting.status ~= "pending") then
                    progressed = true
                    drive(node)
                    drain()
                end
                if root.status ~= "pending" then break end
            end
            if not progressed and root.status == "pending" then
                local timers = false
                for _, node in active.nodes do
                    if node.status == "pending" and (node.kind == "sleep" or node.kind == "timeout" or node.kind == "poll") then timers = true; break end
                end
                assert(idleHost({ timers = timers }), "Task deadlock: no event source can make progress")
            end
        end
    end)
    current = nil
    if root.status == "pending" then terminate(root, "cancelled", nil, "Task session closed") end
    local clean, cleanupFailure = pcall(drain)
    local value, status, rootFailure = root.value, root.status, root.failure
    state = nil
    if not ok then
        error(tostring(failure) .. (status == "failed" and "\n" .. tostring(rootFailure) or "") ..
            (not clean and "\n" .. tostring(cleanupFailure) or ""), 0)
    end
    if not clean then error(cleanupFailure, 0) end
    if status ~= "succeeded" then error(rootFailure or "Task cancelled", 0) end
    return value
end
-- A game host calls step once per frame. Only one session may be active per VM.
export type SessionHandle = { read step: () -> boolean, read close: () -> () }
function api.start<T>(entry: () -> T, adapter: TaskHost?): SessionHandle
    local finished = false
    local started = false
    local host: TaskHost = adapter or { now = native.now }
    local thread = coroutine.create(function()
        api.run(entry, {
            now = host.now,
            update = host.update,
            handle_yield = host.handle_yield,
            idle = function(): boolean coroutine.yield(); return true end,
        })
    end)
    local function step(): boolean
        if finished then return true end
        if not started then assert(not state, "Another task session is active"); started = true end
        local ok, failure = coroutine.resume(thread)
        finished = coroutine.status(thread) == "dead"
        if not ok then error(failure, 0) end
        return finished
    end
    return table.freeze {
        step = step,
        close = function()
            if finished then return end
            if not started then finished = true; return end
            assert(state)
            terminate(state.nodes[1], "cancelled", nil, "Task session closed")
            -- run performs cleanup; cancellation of the root is expected here.
            local ok, failure = pcall(step)
            if not ok and tostring(failure) ~= "Task session closed" then error(failure, 0) end
        end,
    }
end
table.freeze(methods)
export type TaskAPI = typeof(api)
return table.freeze(api)
