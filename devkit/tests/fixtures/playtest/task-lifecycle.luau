local scheduler = require("@playtest/scheduler")
local task = require("@task")

return function()
    local cleanup = {}
    task.on_cleanup(function()
        assert(cleanup.scope == 1 and cleanup.child == 1 and cleanup.root == 1)
        scheduler.host("cleanup", { name = "verified" })
    end)
    task.on_cleanup(function() cleanup.root = (cleanup.root or 0) + 1 end)

    local shared = scheduler.request("shared", {})
    local observed = task.spawn(function() return shared:await() end)
    local cancelledWaiter = task.spawn(function() return shared:await() end)
    task.sleep(0):await() -- Let both waiters start.
    cancelledWaiter:cancel()
    assert(cancelledWaiter:status() == "cancelled")
    assert(shared:status() == "pending")
    local limited = task.timeout(shared, 0.001)
    local ok, failure = pcall(function() limited:await() end)
    assert(not ok and string.find(failure, "Wall timeout", 1, true))
    assert(shared:status() == "pending")
    local won = task.race { immediate = task.sleep(0), shared = shared }:await()
    assert(won.kind == "immediate" and shared:status() == "pending")
    assert(observed:await().answer == 42)
    assert(shared:await() == shared:await(), "Result must be cached")
    assert(shared:status() == "succeeded")

    local source = task.completion()
    local producer = task.spawn(function()
        task.sleep(0):await()
        source.resolve(false)
        source.resolve(true) -- Settles exactly once.
    end)
    assert(source.task:await() == false)
    producer:await()
    local failed = task.spawn(function() error("catchable failure") end)
    ok, failure = pcall(function() failed:await() end)
    assert(not ok and string.find(failure, "catchable failure", 1, true))
    assert(failed:status() == "failed")
    local rejected = task.completion()
    local remaining = task.completion()
    rejected.reject("event failed")
    ok, failure = pcall(function() task.all { rejected = rejected.task, remaining = remaining.task }:await() end)
    assert(not ok and string.find(failure, "event failed", 1, true))
    assert(remaining.task:status() == "pending", "Failed all must detach other inputs")
    remaining.resolve(3)
    assert(remaining.task:await() == 3)

    local selfCancelled, selfCleanup = nil, 0
    selfCancelled = task.spawn(function()
        task.on_cleanup(function() selfCleanup += 1 end)
        selfCancelled:cancel()
        error("Self-cancelled task resumed")
    end)
    ok = pcall(function() selfCancelled:await() end)
    assert(not ok and selfCleanup == 1)
    local cancelCalls = 0
    local cancelledSource = task.completion(function() cancelCalls += 1 end)
    cancelledSource.task:cancel()
    cancelledSource.resolve(99)
    ok = pcall(function() cancelledSource.task:await() end)
    assert(not ok and cancelledSource.task:status() == "cancelled" and cancelCalls == 1)
    local cleanupCalls = 0
    local cleanupFailure = task.spawn(function()
        task.on_cleanup(function() cleanupCalls += 1; error("cleanup failed") end)
        return 1
    end)
    ok, failure = pcall(function() cleanupFailure:await() end)
    assert(not ok and string.find(failure, "cleanup failed", 1, true) and cleanupCalls == 1)

    local child, request
    local ready = task.completion()
    local scope = task.spawn(function()
        task.on_cleanup(function() cleanup.scope = (cleanup.scope or 0) + 1 end)
        child = task.spawn(function()
            task.on_cleanup(function() cleanup.child = (cleanup.child or 0) + 1 end)
            request = scheduler.request("never", {})
            ready.resolve()
            request:await()
        end)
        child:await()
    end)
    ready.task:await()
    scope:cancel()
    task.sleep(0):await()
    assert(scope:status() == "cancelled" and child:status() == "cancelled" and request:status() == "cancelled")
    scope:cancel() -- Idempotent, including cleanup.

    local abandoned
    task.spawn(function() abandoned = scheduler.request("never", {}); return 7 end):await()
    assert(abandoned:status() == "cancelled", "Successful parent exit must close its scope")
    task.sleep(0.03):await() -- No game: wall timers must not request game.advance.
end
