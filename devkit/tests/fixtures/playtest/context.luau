local scheduler = require("@playtest/scheduler")
local context = require("@context")
local game = require("@playtest/game")
local Types = require("./types")

return function()
    local resource = context.resource(Types.State)
    local rows = context.query { state = Types.State }
    local original = Types.State { value = 8 }
    local fixed = context.value(original)
    original.value = 9
    local selected = rows:filter(function(row) return row.state.value > 0 end)
        :order_by(function(row) return row.state.value end):take(2)
    local view = context.view { description = "test context", fields = {
        a = resource:map(function(r) return r.value end):describe("Score"),
        b = context.resource(Types.State),
        nested = context.combine { value = resource },
        selected = selected:map(function(row) return row.entity end),
        first = selected:first(), count = rows:count(),
        empty = rows:take(0),
        missing = context.resource(Types.State):optional(),
        fixed = fixed,
    } }
    local sample = view:sample()
    assert(sample.frame == 12 and sample.simulation_time == 0.2)
    assert(sample.data.a == 42 and sample.data.b.value == 42)
    assert(sample.data.count == 3 and sample.data.first.entity == 2)
    assert(sample.data.selected[1] == 2 and sample.data.selected[2] == 3, "Stable sort")
    assert(sample.data.missing == nil and #sample.data.empty == 0)
    assert(sample.data.fixed.value == 8)
    assert(not pcall(function() sample.data.first.state.value = 0 end))
    assert(not pcall(function() sample.data.selected[1] = 0 end))
    scheduler.host("encoded", { text = sample:encode() })

    local ok, message = pcall(function()
        context.view { fields = { broken = resource:map(function() error("bad projection") end) } }:sample()
    end)
    assert(not ok and string.find(message, "context.broken", 1, true))
    local calls = 0
    ok, message = pcall(function()
        context.view { fields = { broken = resource:map(function()
            calls += 1
            return game:read_resource(Types.State)
        end) } }:sample()
    end)
    assert(not ok and string.find(message, "cannot yield", 1, true) and calls == 1)
    ok = pcall(function()
        context.view { fields = { bad = context.value(function() end) } }:sample()
    end)
    assert(not ok, "Functions cannot be encoded")
    local cyclic = {}; cyclic.self = cyclic
    assert(not pcall(function() context.value(cyclic) end))
end
