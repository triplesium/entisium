local scheduler = require("@playtest/scheduler")
local task = require("@task")
local game = require("@playtest/game")
local Types = require("./types")
local Again = require("./types.luau")

return function()
    assert(Types == Again, "Module imports must share one instance")
    task.on_cleanup(function() scheduler.host("cleanup", { name = "root" }) end)
    local observations = 0
    task.spawn(function()
        task.on_cleanup(function() scheduler.host("cleanup", { name = "child" }) end)
        while true do
            observations += 1
            game.next_step():await()
        end
    end)
    local state = game:read_resource(Types.State)
    assert(state.value == 42)
    assert(not pcall(function() state.value = 1 end), "Snapshots must be read only")
    local waits = task.all { one = game.next_step(), two = game.delay(2 / 60) }
    waits:await()
    -- The root wakes before the child on the second step.
    assert(observations == 2, "Waiters must share the same clock")
    local result = (task.race {
        fast = game.delay(1 / 60), slow = game.delay(1),
    }):await()
    assert(result.kind == "fast")
    local deadline = task.race { condition = game.when(function() return false end), timeout = game.delay(1 / 60) }
    assert(deadline:await().kind == "timeout", "Condition must time out")
end
