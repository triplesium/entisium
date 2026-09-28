local task = require("@task")
-- Exercise the core with a completely different host before the Playtest session starts.
local time, delivered, queue = 0, 0, {}
local coreResult = task.run(function()
    local source = task.completion()
    table.insert(queue, function() source.resolve(17) end)
    local first = task.spawn(function() return source.task:await() end)
    local second = task.spawn(function() return source.task:await() end)
    local values = task.all { first = first, second = second }:await()
    assert(values.first == 17 and values.second == 17)
    task.sleep(0.1):await()
    return time
end, {
    now = function() return time end,
    update = function()
        for _, event in queue do event(); delivered += 1 end
        table.clear(queue)
    end,
    idle = function() time += 50; return true end,
})
return function()
    assert(coreResult == 100 and delivered == 1)
end
