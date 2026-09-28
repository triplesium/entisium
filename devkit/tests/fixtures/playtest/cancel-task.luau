local scheduler = require("@playtest/scheduler")
local task = require("@task")
return function()
    task.on_cleanup(function() scheduler.host("cleanup") end)
    scheduler.request("never", {}):await()
end
