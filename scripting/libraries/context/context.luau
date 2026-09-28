--!strict
export type TypeToken<T> = { read __ets_type: T? }
export type function ContextFields(fields, token)
    local result = types.newtable()
    for key, property in fields:properties() do
        local source = property.read or property.write
        local value = source:readproperty(types.singleton(if token:value() then "__ets_type" else "__context_value"))
        if token:value() and value:is("union") then
            local parts = {}
            for _, part in value:components() do
                if not part:is("nil") then table.insert(parts, part) end
            end
            value = if #parts == 1 then parts[1] else types.unionof(unpack(parts))
        end
        result:setproperty(key, value)
    end
    if token:value() then result:setproperty(types.singleton("entity"), types.number) end
    return result
end
-- Construct recursive fluent types without recursively instantiating aliases
-- with different generic parameters (which Luau does not allow).
export type function ContextNode(value, query)
    local sourceMethods, queryMethods = types.newtable(), types.newtable()
    local function object(v, methods)
        local t = types.newtable()
        t:setreadproperty(types.singleton("__context_value"), v)
        local mt = types.newtable()
        mt:setproperty(types.singleton("__index"), methods)
        t:setmetatable(mt)
        return t
    end
    local function list(v)
        return types.newtable(nil, { index = types.number, readresult = v, writeresult = v })
    end
    local function source(v) return object(v, sourceMethods) end
    local function rows(v) return object(list(v), queryMethods) end
    local function shape(v)
        local t = types.newtable()
        t:setreadproperty(types.singleton("__context_value"), v)
        return t
    end
    local function fn(args, result, generics)
        return types.newfunction({ head = args }, { head = {result} }, generics)
    end
    local T, U = types.generic("Input1"), types.generic("Output1")
    sourceMethods:setproperty(types.singleton("map"), fn({shape(T), fn({T}, U)}, source(U), {T, U}))
    local T = types.generic("Input3")
    sourceMethods:setproperty(types.singleton("describe"), fn({shape(T), types.string}, source(T), {T}))
    local T, U = types.generic("Input4"), types.generic("Output4")
    queryMethods:setproperty(types.singleton("map"), fn({shape(list(T)), fn({T}, U)}, rows(U), {T, U}))
    local T = types.generic("Input5")
    queryMethods:setproperty(types.singleton("filter"), fn({shape(list(T)), fn({T}, types.boolean)}, rows(T), {T}))
    local T = types.generic("Input6")
    queryMethods:setproperty(types.singleton("order_by"), fn({shape(list(T)), fn({T}, types.unionof(types.number, types.string)), types.unionof(types.singleton("asc"), types.singleton("desc"), types.singleton(nil))}, rows(T), {T}))
    local T = types.generic("Input7")
    queryMethods:setproperty(types.singleton("take"), fn({shape(list(T)), types.number}, rows(T), {T}))
    local T = types.generic("Input8")
    queryMethods:setproperty(types.singleton("describe"), fn({shape(list(T)), types.string}, rows(T), {T}))
    local T = types.generic("Input9")
    queryMethods:setproperty(types.singleton("count"), fn({shape(list(T))}, source(types.number), {T}))
    local T = types.generic("Input10")
    queryMethods:setproperty(types.singleton("first"), fn({shape(list(T))}, source(types.unionof(T, types.singleton(nil))), {T}))
    if query:value() == "query" then return rows(value) end
    local result = source(value)
    if query:value() == "resource" then
        result:setproperty(types.singleton("optional"), fn({types.any}, source(types.unionof(value, types.singleton(nil)))))
    end
    return result
end
export type ContextSource<T> = ContextNode<T, "source">
export type ContextResource<T> = ContextNode<T, "resource">
export type ContextQuery<T> = ContextNode<T, "query">
export type ContextSample<T> = {
    read frame: number, read simulation_time: number, read data: T, read schema: Documentation,
    read encode: (ContextSample<T>) -> string,
}
export type ContextView<T> = { read sample: (ContextView<T>) -> ContextSample<T> }
export type Documentation = { description: string?, optional: boolean?, fields: {[string]: Documentation}? }
export type Node = { kind: string, parent: Node?, token: any, tokens: {[string]: any},
    names: {string}, fields: {[string]: Node}, fn: (any) -> any, limit: number,
    description: string?, optional: boolean?, descending: boolean?, value: any }
type NodeFields = { parent: Node?, token: any?, tokens: {[string]: any}?, names: {string}?, fields: {[string]: Node}?,
    fn: ((any) -> any)?, limit: number?, description: string?, optional: boolean?, descending: boolean?, value: any? }
export type Adapter = {
    copy_value: ((any) -> any)?, encode: (any) -> string, request: (Node) -> any,
    sample: ({any}) -> { frame: number, simulation_time: number, batch: {any} },
    decode: (Node, any) -> any,
}
local core = {}
local sourceMethods = {}
local queryMethods = {}
local sourceMt = { __index = sourceMethods }
local queryMt = { __index = queryMethods }
local arrayMt = table.freeze { __context_array = true }

local function array<T>(value: {T}?): {T}
    local result: {T} = value or {}
    setmetatable(result, arrayMt)
    return result
end
local function sortedKeys(value: {[string]: any}): {string}
    local keys: {string} = {}
    for key in value do
        assert(type(key) == "string", "Context field names must be strings")
        table.insert(keys, key)
    end
    table.sort(keys)
    return keys
end
local function copy(value: any, active: {[any]: boolean}?, copyReflected: ((any) -> any)?): any
    if type(value) == "userdata" then
        assert(copyReflected, "Adapter cannot copy reflected values")
        return copyReflected(value)
    end
    if type(value) ~= "table" then return value end
    active = active or {}
    assert(not active[value], "Cyclic context value")
    active[value] = true
    local result = {}
    for k, v in value do result[k] = copy(v, active, copyReflected) end
    if getmetatable(value) == arrayMt then setmetatable(result, arrayMt) end
    active[value] = nil
    return table.freeze(result)
end
local function node(kind: string, fields: NodeFields?, query: boolean?): Node
    local fields: NodeFields = fields or {}
    local result: Node = { kind = kind, parent = fields.parent, token = fields.token, tokens = fields.tokens or {},
        names = fields.names or {}, fields = fields.fields or {}, fn = fields.fn or function(value) return value end,
        limit = fields.limit or 0, description = fields.description, optional = fields.optional,
        descending = fields.descending, value = fields.value }
    return table.freeze(setmetatable(result, if query then queryMt else sourceMt)) :: any
end
local function isNode(value: unknown): boolean
    return type(value) == "table" and (getmetatable(value) == sourceMt or getmetatable(value) == queryMt)
end
local function derived(self: Node, kind: string, fn: (any) -> any, query: boolean?): Node
    assert(type(fn) == "function", kind .. " expects a function")
    return node(kind, { parent = self, fn = fn }, query)
end
local function invoke(fn: (any) -> any, value: any): any
    local thread = coroutine.create(fn)
    local ok, result = coroutine.resume(thread, value)
    if not ok then error(result, 0) end
    assert(coroutine.status(thread) == "dead", "Context callbacks cannot yield or perform host operations")
    return result
end
function sourceMethods.map(self: Node, fn: (any) -> any): Node return derived(self, "map", fn) end
function sourceMethods.describe(self: Node, text: string): Node
    assert(type(text) == "string", "Description must be a string")
    return node("describe", { parent = self, description = text }, getmetatable(self) == queryMt)
end
function sourceMethods.optional(self: Node): Node
    assert(self.kind == "resource", "optional applies directly to a resource source")
    return node("resource", { token = self.token, optional = true })
end
queryMethods.describe = sourceMethods.describe
function queryMethods.map(self: Node, fn: (any) -> any): Node return derived(self, "rows_map", fn, true) end
function queryMethods.filter(self: Node, fn: (any) -> boolean): Node return derived(self, "filter", fn, true) end
function queryMethods.order_by(self: Node, fn: (any) -> (number | string), direction: ("asc" | "desc")?): Node
    assert(direction == nil or direction == "asc" or direction == "desc", "Invalid sort direction")
    assert(type(fn) == "function", "order_by expects a function")
    return node("sort", { parent = self, fn = fn, descending = direction == "desc" }, true)
end
function queryMethods.take(self: Node, count: number): Node
    assert(type(count) == "number" and count >= 0 and count < math.huge and count % 1 == 0, "Invalid take count")
    return node("take", { parent = self, limit = count }, true)
end
function queryMethods.count(self: Node): Node return node("count", { parent = self }) end
function queryMethods.first(self: Node): Node return node("first", { parent = self }) end

-- Node values are evaluated later by the adapter. These constructors carry the
-- type relation between reflected tokens, callbacks and the resulting sample.
function core.resource<T>(token: TypeToken<T>): ContextResource<T> return node("resource", { token = token }) :: any end
function core.query<T>(tokens: T): ContextQuery<ContextFields<T, true>>
    local tokens: {[string]: any} = tokens :: any
    local names = sortedKeys(tokens)
    assert(#names > 0 and tokens.entity == nil, "Query requires components; entity is reserved")
    return node("query", { tokens = table.freeze(table.clone(tokens)), names = table.freeze(names) }, true) :: any
end
function core.value<T>(value: T): ContextSource<T> return node("value", { value = copy(value) }) :: any end
function core.combine<T>(fields: T): ContextSource<ContextFields<T, false>>
    local fields: {[string]: Node} = fields :: any
    for _, field in fields do assert(isNode(field), "combine expects context sources") end
    return node("combine", { fields = table.freeze(table.clone(fields)), names = table.freeze(sortedKeys(fields)) }) :: any
end
core.array = array

function core.bind(adapter: Adapter)
    local api = { resource = core.resource, query = core.query, combine = core.combine, array = core.array }
    function api.value<T>(value: T): ContextSource<T>
        return node("value", { value = copy(value, nil, adapter.copy_value) }) :: any
    end
    function api.view<T>(options: { description: string?, fields: T }): ContextView<ContextFields<T, false>>
        local root: Node = core.combine(options.fields) :: any
        local description = options.description
        local view = {}
        function view.sample(_self: ContextView<ContextFields<T, false>>): ContextSample<ContextFields<T, false>>
            local requests: {any} = {}
            local indices: {[any]: number} = {}
            local visited: {[Node]: boolean} = {}
            local paths: {[Node]: string} = {}
            local function collect(source: Node, path: string): ()
                if visited[source] then return end
                visited[source], paths[source] = true, path
                if source.kind == "resource" or source.kind == "query" then
                    local request = adapter.request(source)
                    local key = adapter.encode(request)
                    local index = indices[key]
                    if not index then
                        table.insert(requests, request)
                        index = #requests
                        indices[key] = index
                    end
                    indices[source] = index
                elseif source.parent then collect(source.parent, path)
                elseif source.kind == "combine" then
                    for _, name in source.names do collect(source.fields[name], path .. "." .. name) end
                end
            end
            collect(root, "context")
            local response = adapter.sample(requests)
            local values: {[Node]: any} = {}
            local evaluated: {[Node]: boolean} = {}
            local evaluate: (Node?) -> any
            local function compute(source: Node): any
                local kind = source.kind
                if kind == "resource" or kind == "query" then
                    return adapter.decode(source, response.batch[indices[source]])
                elseif kind == "value" then return source.value
                elseif kind == "combine" then
                    local result = {}
                    for _, name in source.names do result[name] = evaluate(source.fields[name]) end
                    return result
                end
                local input = evaluate(source.parent)
                if kind == "describe" then return input
                elseif kind == "map" then return invoke(source.fn, input)
                elseif kind == "count" then return #input
                elseif kind == "first" then return input[1] end
                local result: {any} = array()
                if kind == "sort" then
                    local decorated: {{row: any, key: any, index: number}} = {}
                    for i, row in input do decorated[i] = { row = row, key = invoke(source.fn, row), index = i } end
                    table.sort(decorated, function(a: {row: any, key: any, index: number}, b: {row: any, key: any, index: number})
                        if a.key == b.key then return a.index < b.index end
                        if source.descending then return a.key > b.key end
                        return a.key < b.key
                    end)
                    for _, item in decorated do table.insert(result, item.row) end
                else
                    for i, row in input do
                        if kind == "take" and i > source.limit then break end
                        if kind == "rows_map" then
                            local value = invoke(source.fn, row)
                            assert(value ~= nil, "Query map cannot return nil; use filter")
                            table.insert(result, value)
                        elseif kind ~= "filter" or invoke(source.fn, row) then table.insert(result, row) end
                    end
                end
                return result
            end
            evaluate = function(source: Node?): any
                assert(source)
                if evaluated[source] then return values[source] end
                local ok, result = pcall(compute, source)
                if not ok then error((paths[source] or "context") .. ": " .. tostring(result), 0) end
                values[source], evaluated[source] = copy(result, nil, adapter.copy_value), true
                return values[source]
            end
            local function documentation(source: Node?): Documentation
                assert(source)
                local result: Documentation = {}
                if source.kind == "describe" then
                    result = documentation(source.parent)
                    result.description = source.description
                elseif source.kind == "combine" then
                    local fields: {[string]: Documentation} = {}
                    for _, name in source.names do fields[name] = documentation(source.fields[name]) end
                    result.fields = fields
                elseif source.kind == "resource" and rawget(source :: any, "optional") then result.optional = true end
                return result
            end
            local sample = {
                frame = response.frame, simulation_time = response.simulation_time,
                data = evaluate(root), schema = documentation(root),
            }
            sample.schema.description = description
            -- Encode immediately so unsupported values fail at the sampling boundary.
            local encoded = adapter.encode(sample)
            sample.encode = function() return encoded end
            return copy(sample, nil, adapter.copy_value)
        end
        return table.freeze(view)
    end
    return table.freeze(api)
end
export type ContextAPI = typeof(core.bind({} :: Adapter))
return table.freeze(core)
