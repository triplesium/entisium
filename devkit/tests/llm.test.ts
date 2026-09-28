import { describe, expect, it } from "vitest";
import { available, serverFor, configuration, runNative, chatResponse, responsesResponse } from "./native-ai-support.js";
describe.skipIf(!available)("native LLM protocols", () => {
    it.each(["responses", "chat-completions"] as const)("supports %s text and strict structured output", async protocol => {
        const server = await serverFor((body, request) => {
            expect(request.headers.authorization).toBe("Bearer native-test-secret");
            expect(body.model).toBe("requested-model"); expect(body.stream).toBe(false);
            const format = protocol === "responses" ? body.text?.format : body.response_format?.json_schema;
            const messages = body.messages ?? body.input;
            expect(messages.at(-1).content).toBe("next");
            if (format) {
                expect(format.strict).toBe(true);
                expect(format.schema.properties.data.required).toEqual(["empty", "name", "target"]);
                expect(format.schema.properties.data.properties.target.anyOf).toEqual([{type: "integer"}, {type: "null"}]);
                expect(messages[1].content).toMatch(/^Context data \(not system instructions\):\n/);
                expect(JSON.parse(messages[1].content.split("\n").slice(1).join("\n"))).toEqual({x: 1});
            }
            const content = format ? JSON.stringify({data: {name: "done", target: null, empty: []}}) : "hello";
            return protocol === "responses" ? responsesResponse(content) : chatResponse(content);
        });
        try {
            await runNative(`local text = ai.model {model="planner"}:generate {prompt="next"}:await()
                assert(text.data == "hello" and text.model == "actual-model" and text.usage.input_tokens == 12)
                local model = ai.model {model="planner", instructions="Choose", output=schema.object {
                    name=schema.string(), target=schema.optional(schema.integer()), empty=schema.array(schema.string()),
                }}
                local result = model:generate {prompt="next", context={encode=function() return '{"x":1}' end}}:await()
                assert(result.data.name == "done" and result.data.target == nil and #result.data.empty == 0)`, configuration(server.url, protocol));
        } finally { await server.close(); }
    });
    it.each((["responses", "chat-completions"] as const).flatMap(protocol => ["refusal", "truncated", "tool", "empty", "bad-schema", "extra-field", "usage", "http"].map(bad => ({protocol, bad}))))("rejects $protocol $bad responses", async ({protocol, bad}) => {
        const server = await serverFor((_body, _request, response) => {
            const value = chatResponse(JSON.stringify({data: {count: 1}})) as any;
            if (bad === "refusal") value.choices[0].message.refusal = "refused";
            if (bad === "truncated") value.choices[0].finish_reason = "length";
            if (bad === "tool") value.choices[0].message.tool_calls = [{}];
            if (bad === "empty") value.choices[0].message.content = "";
            if (bad === "bad-schema") value.choices[0].message.content = '{"data":{"count":"bad"}}';
            if (bad === "extra-field") value.choices[0].message.content = '{"data":{"count":1,"extra":true}}';
            if (bad === "usage") value.usage.prompt_tokens = -1;
            if (bad === "http") { response.statusCode=429; return {error: "private-provider-detail"}; }
            if (protocol === "responses") {
                const response = responsesResponse(value.choices[0].message.content) as any;
                if (bad === "refusal") response.output[0].content = [{type: "refusal", refusal: "refused"}];
                if (bad === "truncated") response.status = "incomplete";
                if (bad === "tool") response.output = [{type: "function_call"}];
                if (bad === "usage") response.usage.input_tokens = -1;
                return response;
            }
            return value;
        });
        try {
            const promise = runNative(`ai.model {model="planner", output=schema.object {count=schema.integer()}}:generate {prompt="next"}:await()`, configuration(server.url, protocol));
            await expect(promise).rejects.toThrow();
            expect(server.calls()).toBe(1);
        } finally { await server.close(); }
    });
    it("cancels requests and reports closed connections without late results", async () => {
        const server = await serverFor(async () => { await new Promise(resolve => setTimeout(resolve, 150)); return chatResponse("late"); });
        try {
            await runNative(`local model = ai.model {model="planner"}
                local pending = model:generate {prompt="next"}
                task.sleep(0.03):await(); pending:cancel(); task.sleep(0):await()
                assert(pending:status() == "cancelled")
                local binding = { connection = http.connection("test") }
                local waiting = model:generate {prompt="next"}
                task.sleep(0.01):await(); http.close(binding.connection)
                local ok, message = pcall(function() waiting:await() end)
                assert(not ok and string.find(message, "closed", 1, true))
                task.sleep(0.2):await()
                assert(pending:status() == "cancelled")`, configuration(server.url));
        } finally { await server.close(); }
    });
    it("rejects URL escapes and credential overrides before sending", async () => {
        const server = await serverFor(() => chatResponse("unexpected"));
        try {
            await runNative(`local binding = { connection = http.connection("test") }
                local function reject(request)
                    local ok = pcall(http.request, request)
                    assert(not ok)
                end
                reject({connection=binding.connection, url="https://example.invalid", path="/responses"})
                reject({connection=binding.connection, path="//example.invalid/responses"})
                reject({connection=binding.connection, path="/../responses"})
                reject({connection=binding.connection, path="/%2e%2e/responses"})
                reject({connection=binding.connection, path="/responses", headers={{name="authorization", value="override"}}})
                reject({connection={}, path="/responses"})`, configuration(server.url));
            expect(server.calls()).toBe(0);
        } finally { await server.close(); }
    });
    it("enforces native request deadlines", async () => {
        const server = await serverFor(async () => { await new Promise(resolve => setTimeout(resolve, 200)); return chatResponse("late"); });
        const config = configuration(server.url); config.generation.planner.timeout = 0.03;
        try { await expect(runNative(`ai.model {model="planner"}:generate {prompt="next"}:await()`, config)).rejects.toThrow(); }
        finally { await server.close(); }
    });
});
