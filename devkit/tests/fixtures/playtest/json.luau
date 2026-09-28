--!strict
local json = require("@json")
local schema = require("@schema")
local item = schema.object({ name = schema.string(), values = schema.array(schema.number()) })
return {
    run = function()
        local parsed = item:parse(json.decode('{"name":"sample","values":[1,2]}'))
        assert(parsed.name == "sample" and parsed.values[2] == 2)
        assert(json.encode(json.decode('[null,[],{}]')) == '[null,[],{}]')
        local absent = item:safe_parse(json.decode('{"name":null,"values":[]}'))
        assert(not absent.success)
        local optional = schema.object({name = schema.optional(schema.string())})
        assert(optional:safe_parse(json.decode('{}')).success)
        assert(not optional:safe_parse(json.decode('{"name":null}')).success)
    end,
}
