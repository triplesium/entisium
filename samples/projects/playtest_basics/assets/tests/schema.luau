local schema = require("@schema")

local Command = schema.object {
    action = schema.enum { "move", "stop" }:describe("Requested movement"),
    target = schema.optional(schema.object { x = schema.number(), y = schema.number() }),
    labels = schema.array(schema.string()),
}

return function()
    local command = Command:parse { action = "move", target = { x = 3, y = 2 }, labels = {} }
    assert(command.action == "move")
    assert(command.target and command.target.x == 3)
    local invalid = Command:safe_parse { action = "jump", labels = {} }
    assert(not invalid.success)
    assert(invalid.issues[1].path[1] == "action")
    assert(Command:definition().fields.action.description == "Requested movement")
end
