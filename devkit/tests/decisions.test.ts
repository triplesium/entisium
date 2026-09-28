import { describe, expect, it } from "vitest";
import { available, serverFor, configuration, runNative } from "./native-ai-support.js";
const result = {model: "jev-pinned-test", answers: {
    move: {type: "choice", choice: "right", probabilities: {right: 0.9, stay: 0.1}, confidence: 0.8},
    danger: {type: "noul", noul: 0.2},
    progress: {type: "score", score: 0.6, probabilities: {"0": 0.4, "1": 0.6}, confidence: 0.1, legend: {"0": "start", "1": "done"}},
}, usage: {input_tokens: 10, output_tokens: 5}};
const call = `local policy = ai.decision {questions = {
    move = ai.choice {instructions = "Move?", criteria = {right = "increase", stay = "hold"}},
    danger = ai.noul {instructions = "Danger?"},
    progress = ai.score {instructions = "Progress?", criteria = {"start", "done"}},
}}
local answer = policy:evaluate({encode = function() return '{"data":{"x":1}}' end}):await()`;
describe.skipIf(!available)("native Jev protocol", () => {
    it("sends structured context and preserves fractional score and metadata", async () => {
        const server = await serverFor((body, request) => {
            expect(request.url).toBe("/v1/systemone");
            expect(request.headers.authorization).toBe("Bearer native-test-secret");
            expect(body.model).toBe("jev-latest"); expect(body.state).toEqual({data: {x: 1}});
            expect(body.questions.progress.criteria).toEqual(["start", "done"]);
            return result;
        });
        try { await runNative(call + `\nassert(answer.answers.progress.score == 0.6 and answer.usage.input_tokens == 10)`, configuration(server.url, "jev")); }
        finally { await server.close(); }
    });
    it.each(["type", "choice", "probabilities", "legend", "noul", "usage", "missing"])("rejects invalid %s", async bad => {
        const value = structuredClone(result) as any;
        if (bad === "type") value.answers.move.type = "score";
        if (bad === "choice") value.answers.move.choice = "unknown";
        if (bad === "probabilities") value.answers.move.probabilities.right = 0.3;
        if (bad === "legend") value.answers.progress.legend["0"] = "wrong";
        if (bad === "noul") value.answers.danger.noul = 2;
        if (bad === "usage") value.usage.input_tokens = -1;
        if (bad === "missing") delete value.answers.danger;
        const server = await serverFor(() => value);
        try { await expect(runNative(call, configuration(server.url, "jev"))).rejects.toThrow(); }
        finally { await server.close(); }
    });
});
