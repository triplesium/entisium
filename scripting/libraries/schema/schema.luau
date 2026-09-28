--!strict
export type SchemaIssue = { read path: {string | number}, read message: string }
export type SchemaResult<T> = { read success: true, read data: T } | { read success: false, read issues: {SchemaIssue} }
export type SchemaDefinition = {
    read kind: string, read description: string?, read fields: {[string]: SchemaDefinition}?,
    read element: SchemaDefinition?, read inner: SchemaDefinition?, read values: {string}?,
}
export type Schema<T> = {
    __schema_value: T,
    parse: (Schema<T>, unknown) -> T,
    safe_parse: (Schema<T>, unknown) -> SchemaResult<T>,
    describe: (Schema<T>, string) -> Schema<T>,
    definition: (Schema<T>) -> SchemaDefinition,
}
export type function SchemaFields(fields)
    local result = types.newtable()
    for key, property in fields:properties() do
        local source = property.read or property.write
        result:setproperty(key, source:readproperty(types.singleton("__schema_value")))
    end
    return result
end
export type function SchemaValue(source)
    return source:readproperty(types.singleton("__schema_value"))
end
-- Pure data schemas. No host, ECS, context or model dependencies.
local schema = {}
type Spec = { read kind: string, read description: string?, read fields: {[string]: Schema<any>}?,
    read inner: Schema<any>?, read element: Schema<any>?, read values: {string}? }
local nodes: {[any]: Spec} = setmetatable({}, { __mode = "k" })
local methods = {}
local function make(spec: Spec): Schema<any>
    local instance = table.freeze(setmetatable({}, table.freeze { __index = methods }))
    nodes[instance] = table.freeze(spec)
    -- Phantom T is represented by the descriptor and checked by parse.
    return instance :: any
end

local function node(value: unknown): Spec
    local result = nodes[value]
    assert(result, "Expected a schema")
    return result
end

local function plain(value: unknown): boolean
    return type(value) == "table" and getmetatable(value) == nil
end

local function dense(value: any): (boolean, number)
    if not plain(value) then return false, 0 end
    local count = 0
    for key in value do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then return false, 0 end
        count += 1
    end
    for i = 1, count do if value[i] == nil then return false, 0 end end
    return true, count
end

local function keys(value: {[string]: Schema<any>}): {string}
    local result: {string} = {}
    for key in value do table.insert(result, key) end
    table.sort(result)
    return result
end

local function validate<T>(root: Schema<T>, input: unknown): SchemaResult<T>
    local issues: {SchemaIssue} = {}
    local path: {string | number} = {}
    local active: {[any]: boolean} = {}
    local visited = 0
    local function issue(message: string): ()
        -- One precise error is sufficient; avoid unbounded error accumulation.
        if #issues == 0 then
            table.insert(issues, table.freeze { path = table.freeze(table.clone(path)), message = message })
        end
    end
    -- Values are heterogeneous while recursively validating; only a successful
    -- traversal may expose the schema's generic output type.
    local visit: (Spec, any, number) -> any
    visit = function(spec: Spec, value: any, depth: number): any
        visited += 1
        if depth > 64 or visited > 10000 then issue("Validation limit exceeded"); return nil end
        local kind = spec.kind
        if kind == "optional" then
            if value == nil then return nil end
            return visit(node(spec.inner), value, depth + 1)
        end
        if kind == "string" or kind == "boolean" then
            if type(value) ~= kind then issue("Expected " .. kind); return nil end
            return value
        elseif kind == "number" or kind == "integer" then
            if type(value) ~= "number" or value ~= value or math.abs(value) == math.huge then
                issue("Expected finite " .. kind); return nil
            end
            if kind == "integer" and value % 1 ~= 0 then issue("Expected integer"); return nil end
            return value
        elseif kind == "enum" then
            assert(spec.values)
            for _, option in spec.values do if value == option then return value end end
            issue("Expected an enum member"); return nil
        end
        if not plain(value) then issue("Expected plain " .. kind .. " table"); return nil end
        if active[value] then issue("Cyclic value"); return nil end
        active[value] = true
        local output: {[any]: any} = {}
        if kind == "array" then
            local valid, length = dense(value)
            if not valid then issue("Expected dense array")
            elseif length > 10000 - visited then issue("Validation limit exceeded")
            else
                for i = 1, length do
                    table.insert(path, i)
                    output[i] = visit(node(spec.element), value[i], depth + 1)
                    table.remove(path)
                    if #issues > 0 then break end
                end
            end
        else
            assert(spec.fields)
            local unknown: {string} = {}
            for key in value do
                if type(key) ~= "string" then issue("Expected string object keys"); break end
                if spec.fields[key] == nil then table.insert(unknown, key) end
            end
            table.sort(unknown)
            if #issues == 0 and #unknown > 0 then
                table.insert(path, unknown[1]); issue("Unknown field"); table.remove(path)
            end
            if #issues == 0 then
                for _, key in keys(spec.fields) do
                    table.insert(path, key)
                    output[key] = visit(node(spec.fields[key]), value[key], depth + 1)
                    table.remove(path)
                    if #issues > 0 then break end
                end
            end
        end
        active[value] = nil
        return table.freeze(output)
    end
    local data = visit(node(root), input, 0)
    if #issues > 0 then
        local failure: SchemaResult<T> = { success = false, issues = table.freeze(issues) }
        return table.freeze(failure)
    end
    local success: SchemaResult<T> = { success = true, data = data }
    return table.freeze(success)
end

function methods.safe_parse<T>(self: Schema<T>, value: unknown): SchemaResult<T> return validate(self, value) end
function methods.parse<T>(self: Schema<T>, value: unknown): T
    local result = validate(self, value)
    if result.success then return result.data end
    local issue = result.issues[1]
    local path = "$"
    for _, part in issue.path do
        path ..= if type(part) == "number" then "[" .. tostring(part) .. "]" else "[" .. string.format("%q", part) .. "]"
    end
    error(path .. ": " .. issue.message, 2)
end
function methods.describe<T>(self: Schema<T>, description: string): Schema<T>
    assert(type(description) == "string", "Expected description string")
    local original = node(self)
    local spec: Spec = { kind = original.kind, description = description, fields = original.fields,
        inner = original.inner, element = original.element, values = original.values }
    return make(spec)
end
function methods.definition<T>(self: Schema<T>): SchemaDefinition
    local visited = 0
    local function export(spec: Spec, depth: number): SchemaDefinition
        visited += 1
        assert(depth <= 64 and visited <= 10000, "Schema definition limit exceeded")
        local result: { kind: string, description: string?, values: {string}?, fields: {[string]: SchemaDefinition}?, element: SchemaDefinition?, inner: SchemaDefinition? } = { kind = spec.kind, description = spec.description, values = spec.values }
        if spec.fields then
            local fields: {[string]: SchemaDefinition} = {}
            for key, child in spec.fields do fields[key] = export(node(child), depth + 1) end
            result.fields = table.freeze(fields)
        end
        if spec.element then result.element = export(node(spec.element), depth + 1) end
        if spec.inner then result.inner = export(node(spec.inner), depth + 1) end
        return table.freeze(result)
    end
    return export(node(self), 0)
end
table.freeze(methods)
function schema.string(): Schema<string> return make { kind = "string" } end
function schema.number(): Schema<number> return make { kind = "number" } end
function schema.integer(): Schema<number> return make { kind = "integer" } end
function schema.boolean(): Schema<boolean> return make { kind = "boolean" } end
function schema.optional<T>(inner: T): Schema<SchemaValue<T>?>
    node(inner)
    return make { kind = "optional", inner = inner }
end
function schema.array<T>(element: T): Schema<{SchemaValue<T>}>
    node(element)
    return make { kind = "array", element = element }
end
function schema.object<T>(fields: T): Schema<SchemaFields<T>>
    assert(plain(fields), "Expected schema fields table")
    local copy: {[string]: Schema<any>} = {}
    for key, child in fields :: any do
        assert(type(key) == "string", "Expected string field names")
        node(child)
        copy[key] = child
    end
    return make { kind = "object", fields = table.freeze(copy) }
end
function schema.enum(values: {string}): Schema<string>
    local valid, length = dense(values)
    assert(valid and length > 0, "Expected nonempty dense enum array")
    local seen, copy = {}, {}
    for _, value in values do
        assert(type(value) == "string" and not seen[value], "Expected unique string enum members")
        seen[value] = true
        table.insert(copy, value)
    end
    return make { kind = "enum", values = table.freeze(copy) }
end
export type SchemaAPI = typeof(schema)
return table.freeze(schema)
