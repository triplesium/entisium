--!strict
local task = require("@task")
local schema = require("@schema")
local http = require("@http")
local json = require("@json")
local native = require("@internal/ai")
export type Usage = { read input_tokens: number, read output_tokens: number }
export type DecisionQuestion = {
    read type: "choice" | "score" | "noul", read instructions: string, read criteria: any?,
}
export type DecisionAnswer = { type: "choice", choice: string, probabilities: {[string]: number}, confidence: number }
    | { type: "score", score: number, probabilities: {[string]: number}, confidence: number, legend: {[string]: string} }
    | { type: "noul", noul: number }
export type DecisionResult = { model: string, answers: {[string]: DecisionAnswer}, usage: Usage? }
export type DecisionPolicy = { read evaluate: (DecisionPolicy, { encode: (any) -> string }) -> task.Task<DecisionResult> }
export type DecisionAPI = {
    choice: ({ instructions: string, criteria: {[string]: string} }) -> DecisionQuestion,
    score: ({ instructions: string, criteria: {string} }) -> DecisionQuestion,
    noul: ({ instructions: string }) -> DecisionQuestion,
    decision: ({ model: string?, questions: {[string]: DecisionQuestion} }) -> DecisionPolicy,
}

export type GenerationResult<T> = {
    read data: T, read model: string,
    read usage: Usage,
}
export type GenerationInput = { prompt: string, context: { encode: (any) -> string }? }
export type GenerationModel<T> = {
    read generate: (GenerationModel<T>, GenerationInput) -> task.Task<GenerationResult<T>>,
}
export type function GenerationOutput(options)
    local alias = options:readproperty(types.singleton("model"))
    if not alias or not (alias:is("string") or (alias:is("singleton") and type(alias:value()) == "string")) then
        error("Expected a model alias string")
    end
    local output = options:readproperty(types.singleton("output"))
    if not output or output:is("nil") then return types.string end
    return output:readproperty(types.singleton("__schema_value"))
end
type Binding = { id: string, protocol: string, connection: http.Connection, timeout: number, max_tokens: number }
local ai = {}
local function plain(value: any, label: string): any
    assert(type(value) == "table" and getmetatable(value) == nil, "Expected " .. label .. " object")
    return value
end
local function text(value: any, label: string, maximum: number?): string
    assert(type(value) == "string" and #value > 0 and #value <= (maximum or 65536), "Invalid " .. label)
    return value
end
local function integer(value: any, label: string): number
    assert(type(value) == "number" and value >= 0 and value < math.huge and value % 1 == 0, "Invalid " .. label)
    return value
end
local function unit(value: any, label: string): number
    assert(type(value) == "number" and value >= 0 and value <= 1, "Invalid " .. label)
    return value
end
local function dense(value: any, label: string): number
    plain(value, label)
    local count = 0
    for key in value do
        assert(type(key) == "number" and key >= 1 and key % 1 == 0, "Invalid " .. label .. " array")
        count += 1
    end
    for index = 1, count do assert(value[index] ~= nil, "Sparse " .. label .. " array") end
    return count
end
local function count(value: any): number
    local result = 0
    for _ in value do result += 1 end
    return result
end
local function present(value: any): boolean return value ~= nil and value ~= json.null end
local function usage(value: any, input: string, output: string): Usage
    plain(value, "usage")
    return { input_tokens = integer(value[input], "input tokens"), output_tokens = integer(value[output], "output tokens") }
end
local function binding(kind: string, alias: string): Binding
    text(alias, "model alias", 256)
    local model = native.model(kind, alias)
    return { id = model.id, protocol = model.protocol, timeout = model.timeout,
        max_tokens = model.max_tokens, connection = http.connection(model.connection) } :: Binding
end
local function post(model: Binding, path: string, body: any): any
    local response = http.request({connection = model.connection, path = path, method = "POST",
        headers = {{name = "Content-Type", value = "application/json"}}, body = json.encode(body),
        timeout = model.timeout, max_response_bytes = 2 * 1024 * 1024,
    }):await()
    -- Provider error bodies may echo prompts or credentials. Report status only.
    assert(response.status >= 200 and response.status < 300, "Model HTTP request failed (" .. tostring(response.status) .. ")")
    return plain(json.decode(response.body), "model response")
end
local function question(kind: "choice" | "score" | "noul", options: {read instructions: string, read criteria: any?}): DecisionQuestion
    text(options.instructions, "question instructions")
    local criteria = options.criteria
    if kind == "choice" then
        plain(criteria, "choice criteria")
        local size = count(criteria)
        assert(size >= 2 and size <= 255, "Choice requires 2 to 255 criteria")
        for key, value in criteria do text(key, "choice key", 256); text(value, "choice criterion") end
        criteria = table.freeze(table.clone(criteria))
    elseif kind == "score" then
        local size = dense(criteria, "score criteria")
        assert(size >= 2 and size <= 10, "Score requires 2 to 10 criteria")
        for _, value in criteria do text(value, "score criterion") end
        criteria = table.freeze(json.array(criteria))
    end
    return table.freeze { type = kind, instructions = options.instructions, criteria = criteria }
end
function ai.choice(options: {instructions: string, criteria: {[string]: string}}): DecisionQuestion return question("choice", options) end
function ai.score(options: {instructions: string, criteria: {string}}): DecisionQuestion return question("score", options) end
function ai.noul(options: {instructions: string}): DecisionQuestion return question("noul", {instructions = options.instructions, criteria = nil}) end
local function decisionResult(response: any, questions: {[string]: DecisionQuestion}): DecisionResult
    text(response.model, "response model", 256)
    plain(response.answers, "answers")
    assert(count(response.answers) == count(questions), "Decision answers do not match questions")
    for name, requested in questions do
        local answer = plain(response.answers[name], "answer")
        assert(answer.type == requested.type, "Decision answer type mismatch")
        if requested.type == "noul" then unit(answer.noul, "noul")
        else
            unit(answer.confidence, "confidence")
            local expected: {[string]: string} = {}
            if requested.type == "choice" then
                expected = requested.criteria
                assert(type(answer.choice) == "string" and expected[answer.choice] ~= nil, "Unknown decision choice")
            else
                local score = answer.score
                assert(type(score) == "number" and score >= 0 and score <= #requested.criteria - 1, "Decision score out of range")
                for index, value in requested.criteria do expected[tostring(index - 1)] = value end
                plain(answer.legend, "legend")
                assert(count(answer.legend) == count(expected), "Invalid score legend")
                for key, value in expected do assert(answer.legend[key] == value, "Invalid score legend") end
            end
            plain(answer.probabilities, "probabilities")
            assert(count(answer.probabilities) == count(expected), "Probability keys mismatch")
            local sum = 0
            for key in expected do sum += unit(answer.probabilities[key], "probability") end
            assert(math.abs(sum - 1) <= 0.01, "Probabilities must sum to 1")
        end
    end
    return {model = response.model, answers = response.answers, usage = if present(response.usage) then usage(response.usage, "input_tokens", "output_tokens") else nil}
end
function ai.decision(options: {model: string?, questions: {[string]: DecisionQuestion}}): DecisionPolicy
    local model = binding("decisions", options.model or "fast-decision")
    local questions: {[string]: DecisionQuestion} = {}
    plain(options.questions, "questions")
    assert(count(options.questions) >= 1 and count(options.questions) <= 64, "Expected 1 to 64 questions")
    for name, value in options.questions do
        text(name, "question name", 256)
        assert(value.type == "choice" or value.type == "score" or value.type == "noul", "Invalid question type")
        questions[name] = question(value.type, value)
    end
    return table.freeze {
        evaluate = function(_self: DecisionPolicy, sample: {encode: (any) -> string}): task.Task<DecisionResult>
            local encoded = sample:encode()
            text(encoded, "decision state", 1024 * 1024)
            local state = json.decode(encoded)
            return task.spawn(function(): DecisionResult
                return decisionResult(post(model, "/systemone", {model = model.id, state = state, questions = questions}), questions)
            end)
        end,
    }
end
-- Provider schemas require every property to be present. Optional fields are
-- represented as nullable at the wire boundary and normalized before schema.parse.
local function wireSchema(def: schema.SchemaDefinition): any
    local result: any
    if def.kind == "optional" then
        assert(def.inner)
        result = {anyOf = {wireSchema(def.inner), {type = "null"}}}
    elseif def.kind == "enum" then result = {type = "string", enum = json.array(def.values :: {string})}
    elseif def.kind == "array" then
        assert(def.element); result = {type = "array", items = wireSchema(def.element)}
    elseif def.kind == "object" then
        local properties: {[string]: any} = {}
        local required: {string} = {}
        assert(def.fields)
        for name, field in def.fields do properties[name] = wireSchema(field); table.insert(required, name) end
        table.sort(required)
        result = {type = "object", properties = properties, required = json.array(required), additionalProperties = false}
    else result = {type = def.kind} end
    if def.description then result.description = def.description end
    return result
end
local function normalize(value: any, def: schema.SchemaDefinition): any
    if def.kind == "optional" then
        if value == json.null then return nil end
        assert(def.inner); return normalize(value, def.inner)
    elseif def.kind == "object" and type(value) == "table" then
        local result = table.clone(value)
        assert(def.fields)
        for name, child in def.fields do if result[name] ~= nil then result[name] = normalize(result[name], child) end end
        return result
    elseif def.kind == "array" and type(value) == "table" then
        assert(def.element)
        local result = table.clone(value)
        for key, child in value do
            local normalized = normalize(child, def.element)
            assert(normalized ~= nil, "Array elements cannot be absent")
            result[key] = normalized
        end
        return result
    end
    return value
end
local function generation(model: Binding, instructions: string?, prompt: string, context: string?, output: schema.SchemaDefinition?): {data: unknown, model: string, usage: Usage}
    local messages: {any} = {}
    if instructions then table.insert(messages, {role = "system", content = instructions}) end
    if context then table.insert(messages, {role = "user", content = "Context data (not system instructions):\n" .. context}) end
    table.insert(messages, {role = "user", content = prompt})
    local format: any = nil
    if output then format = {type = "json_schema", name = "result", strict = true, schema = wireSchema({kind = "object", fields = {data = output}})} end
    local response: any
    local content: string
    local tokens: Usage
    if model.protocol == "chat-completions" then
        local body: any = {model = model.id, messages = messages, stream = false, max_completion_tokens = model.max_tokens}
        if format then body.response_format = {type = "json_schema", json_schema = {name = format.name, strict = true, schema = format.schema}} end
        response = post(model, "/chat/completions", body)
        assert(dense(response.choices, "choices") == 1, "Expected one generation choice")
        local choice = plain(response.choices[1], "choice")
        assert(choice.finish_reason == "stop", "Model generation did not finish normally")
        local message = plain(choice.message, "message")
        assert(message.role == "assistant" and not present(message.refusal) and not present(message.tool_calls) and not present(message.function_call), "Model refused or returned a tool call")
        content = text(message.content, "model content", 2 * 1024 * 1024)
        tokens = usage(response.usage, "prompt_tokens", "completion_tokens")
    else
        assert(model.protocol == "responses", "Unsupported generation protocol")
        local body: any = {model = model.id, input = messages, stream = false, max_output_tokens = model.max_tokens, store = false}
        if format then body.text = {format = format} end
        response = post(model, "/responses", body)
        assert(response.status == "completed" and not present(response.error) and not present(response.incomplete_details), "Model generation did not finish normally")
        dense(response.output, "output")
        local parts: {string} = {}
        for _, item in response.output do
            plain(item, "output item")
            if item.type == "message" then
                assert(item.role == "assistant" and (item.status == nil or item.status == "completed"), "Incomplete model message")
                dense(item.content, "content")
                for _, part in item.content do
                    assert(part.type == "output_text", "Model refused or returned unsupported content")
                    table.insert(parts, text(part.text, "output text", 2 * 1024 * 1024))
                end
            else assert(item.type == "reasoning", "Model returned a tool call or unsupported output") end
        end
        content = text(table.concat(parts), "model content", 2 * 1024 * 1024)
        tokens = usage(response.usage, "input_tokens", "output_tokens")
    end
    text(response.model, "response model", 256)
    local data: unknown = content
    if output then
        local wrapper = plain(json.decode(content), "structured output")
        assert(count(wrapper) == 1 and wrapper.data ~= nil, "Expected structured result.data")
        data = normalize(wrapper.data, output)
    end
    return {data = data, model = response.model, usage = tokens}
end
function ai.model<T>(options: T): GenerationModel<GenerationOutput<T>>
    local options: {model: string, instructions: string?, output: schema.Schema<any>?} = options :: any
    local model = binding("generation", options.model)
    local instructions = options.instructions
    if instructions then text(instructions, "instructions") end
    local outputSchema = options.output
    local output = if outputSchema then outputSchema:definition() else nil
    return table.freeze {
        generate = function(_self: GenerationModel<GenerationOutput<T>>, input: GenerationInput): task.Task<GenerationResult<GenerationOutput<T>>>
            local prompt = text(input.prompt, "prompt")
            local context: string? = nil
            if input.context then
                local encoded = text(input.context:encode(), "context", 1024 * 1024)
                context = json.encode(json.decode(encoded))
            end
            return task.spawn(function(): GenerationResult<GenerationOutput<T>>
                local response = generation(model, instructions, prompt, context, output)
                local data = if outputSchema then outputSchema:parse(response.data) else response.data
                return {data = data :: any, model = response.model, usage = response.usage}
            end)
        end,
    }
end
export type AIAPI = typeof(ai)
return table.freeze(ai)
