local schema = require("@schema")

return function()
    assert(schema.object { __proto__ = schema.string() }:parse { __proto__ = "value" }.__proto__ == "value")
    local options = { "attack", "retreat" }
    local fields = {
        action = schema.enum(options):describe("Next action"),
        target = schema.optional(schema.integer()),
        points = schema.array(schema.object { x = schema.number(), visible = schema.boolean() }),
    }
    local decision = schema.object(fields):describe("Decision")
    options[1] = "changed"
    fields.extra = schema.string()
    local input = { action = "attack", points = { { x = 1, visible = true } } }
    local parsed = decision:parse(input)
    input.points[1].x = 99
    assert(parsed.points[1].x == 1 and parsed.target == nil)
    assert(table.isfrozen(parsed) and table.isfrozen(parsed.points[1]))
    local definition = decision:definition()
    assert(definition.description == "Decision")
    assert(definition.fields.action.description == "Next action")
    assert(definition.fields.action.values[1] == "attack")
    assert(definition.fields.target.kind == "optional")
    assert(definition.fields.extra == nil)
    assert(schema.string():definition().description == nil)
    assert(schema.optional(schema.string()):safe_parse(nil).success)
    assert(schema.boolean():parse(false) == false)
    assert(#schema.array(schema.string()):parse({}) == 0)

    local function bad(spec, value, expected)
        local result = spec:safe_parse(value)
        assert(not result.success)
        assert(result.issues[1].message == expected, result.issues[1].message)
        assert(not pcall(function() spec:parse(value) end))
        return result.issues[1]
    end
    local issue = bad(decision, { action = "attack", points = { { x = "secret", visible = true } } }, "Expected finite number")
    assert(issue.path[1] == "points" and issue.path[2] == 1 and issue.path[3] == "x")
    local ok, message = pcall(function() decision:parse { action = "attack", points = { { x = "secret", visible = true } } } end)
    assert(not ok and string.find(message, '$["points"][1]["x"]', 1, true))
    assert(not string.find(message, "secret", 1, true))
    bad(decision, { action = "attack", points = {}, extra = 1 }, "Unknown field")
    bad(schema.number(), 0 / 0, "Expected finite number")
    bad(schema.number(), math.huge, "Expected finite number")
    bad(schema.integer(), 1.5, "Expected integer")
    bad(schema.string(), nil, "Expected string")
    bad(schema.enum { "a" }, "b", "Expected an enum member")
    bad(schema.array(schema.string()), { [2] = "a" }, "Expected dense array")
    bad(schema.array(schema.string()), { a = "a" }, "Expected dense array")
    bad(schema.object {}, { [1] = 1 }, "Expected string object keys")
    bad(schema.object {}, setmetatable({}, {}), "Expected plain object table")
    local cyclic = {}; cyclic.child = cyclic
    bad(schema.object { child = schema.object {} }, cyclic, "Cyclic value")
    local nested, value = schema.string(), "leaf"
    for _ = 1, 66 do nested = schema.array(nested); value = { value } end
    bad(nested, value, "Validation limit exceeded")
    local large = table.create(10001, 1)
    bad(schema.array(schema.number()), large, "Validation limit exceeded")
    for _, create in {
        function() schema.enum {} end,
        function() schema.enum { "a", "a" } end,
        function() schema.enum { 1 } end,
        function() schema.object { x = {} } end,
        function() schema.array({}) end,
    } do assert(not pcall(create)) end
end
