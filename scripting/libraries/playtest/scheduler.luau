--!strict
local http = require("@http")
local task = require("@task")
local scheduler = {}
type Wait = { source: task.TaskCompletion<nil>, predicate: () -> boolean }
local external: {[string]: task.TaskCompletion<any>} = {}
local waits: {Wait} = {}
local now, step = 0, 0

function scheduler.host(method: string, payload: any?): any
    local reply = coroutine.yield({ kind = "request", method = method, payload = payload or {} })
    assert(type(reply) == "table" and type(reply.ok) == "boolean", "Invalid host response")
    if not reply.ok then error(reply.error, 2) end
    return reply.value
end
local function waitFor(predicate: () -> boolean): task.Task<nil>
    local source = task.completion() :: task.TaskCompletion<nil>
    table.insert(waits, { source = source, predicate = predicate })
    return source.task
end
function scheduler.delay(seconds: number): task.Task<nil>
    assert(type(seconds) == "number" and seconds >= 0 and seconds < math.huge, "Invalid duration")
    local deadline = now + seconds
    return waitFor(function() return now + 1e-9 >= deadline end)
end
function scheduler.next_step(): task.Task<nil>
    local target = step + 1
    return waitFor(function() return step >= target end)
end
function scheduler.when(predicate: () -> boolean): task.Task<nil>
    assert(type(predicate) == "function", "Expected predicate")
    return waitFor(predicate)
end
function scheduler.request(method: string, payload: unknown): task.Task<unknown>
    local id: string? = nil
    local source = task.completion(function()
        if id then scheduler.host("task.cancel", { id = id }) end
    end) :: task.TaskCompletion<unknown>
    id = scheduler.host("task.submit", { method = method, payload = payload }).id
    assert(id)
    external[id] = source
    return source.task
end
function scheduler.run<T>(entry: () -> T): T
    table.clear(external); table.clear(waits)
    now, step = 0, 0
    return task.run(entry, {
        now = function() return scheduler.host("task.clock").now_ms end,
        handle_yield = function(event: any): any
            assert(type(event) == "table" and event.kind == "request", "Unmanaged coroutine yield")
            return coroutine.yield(event)
        end,
        update = function()
            local ids = {}
            for id, source in external do
                if source.task:status() == "pending" then table.insert(ids, id) else external[id] = nil end
            end
            table.sort(ids)
            if #ids > 0 then
                local poll = scheduler.host("task.poll", { ids = ids })
                for id, result in poll.states do
                    if result.status == "completed" then external[id].resolve(result.value, result.settled_ms)
                    elseif result.status == "failed" then external[id].reject(result.error, result.settled_ms) end
                end
            end
            for i = #waits, 1, -1 do
                local wait = waits[i]
                if wait.source.task:status() == "pending" then
                    local ok, ready = pcall(wait.predicate)
                    if not ok then wait.source.reject(tostring(ready))
                    elseif ready then wait.source.resolve() end
                end
                if wait.source.task:status() ~= "pending" then table.remove(waits, i) end
            end
        end,
        idle = function(info: {timers: boolean}): boolean
            local ids = {}
            for id, source in external do if source.task:status() == "pending" then table.insert(ids, id) end end
            if #ids > 0 then scheduler.host("task.wait", { ids = ids }); return true end
            if http.pending() > 0 then scheduler.host("task.idle"); return true end
            local gameWaits = false
            for _, wait in waits do if wait.source.task:status() == "pending" then gameWaits = true; break end end
            if gameWaits then
                local result = scheduler.host("game.advance", { ticks = 1 })
                assert(result.ticks == 1 and not result.stopped, "Game stopped before the wait completed")
                now += result.delta; step += 1
                return true
            end
            if info.timers then scheduler.host("task.idle"); return true end
            return false
        end,
    })
end
return table.freeze(scheduler)
