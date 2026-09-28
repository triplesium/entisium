import { imageOptionsSchema } from "../contracts/image-generation.js";
import { z } from "zod/v4";

const text = z.string().trim().min(1);
const baseUrl = z.string().url().refine((value) => {
    const url = new URL(value);
    return ["http:", "https:"].includes(url.protocol) && !url.username && !url.password && !url.search && !url.hash;
});
export const modelSelectionSchema = z.object({ provider: text, id: text }).strict();
const providerSchema = z.object({
    type: z.enum(["openai", "openrouter", "fal", "typesafe", "openai-compatible"]),
    name: text.optional(),
    apiKey: z.string().trim().min(8).max(4096).regex(/^[\x21-\x7e]+$/).optional(),
    decisions: z.object({ baseUrl: baseUrl.optional() }).strict().optional(),
    chat: z.object({ api: z.enum(["responses", "chat-completions"]).optional(), baseUrl: baseUrl.optional() }).strict().optional(),
    images: z.object({
        api: z.enum(["openai-images", "openrouter-images", "fal-images"]).optional(),
        baseUrl: baseUrl.optional(),
    }).strict().optional(),
    // Optional chat catalogue/metadata overrides, interpreted by Agent.
    models: z.array(z.unknown()).optional(),
}).strict();

export const configSchema = z.object({
    version: z.literal(1),
    providers: z.record(z.string().regex(/^[A-Za-z0-9][A-Za-z0-9._-]*$/), providerSchema).default({}),
    imageGeneration: z.object({
        model: modelSelectionSchema.default({ provider: "openai", id: "gpt-image-2" }),
        timeoutMs: z.number().int().min(1000).max(30 * 60_000).default(10 * 60_000),
        defaults: imageOptionsSchema.prefault({}),
    }).strict().prefault({}),
    runtime: z.object({ executable: text.default("auto") }).prefault({}),
    llm: z.object({
        models: z.record(text, modelSelectionSchema),
        timeoutMs: z.number().int().min(1).max(300_000).default(30_000),
        maxTokens: z.number().int().min(1).max(65536).default(4096),
    }).strict().optional(),
    decisions: z.object({
        models: z.record(text, modelSelectionSchema).default({ "fast-decision": { provider: "typesafe", id: "jev-latest" } }),
        timeoutMs: z.number().int().min(1).max(300_000).default(30_000),
    }).strict().optional(),
    agent: z.unknown().optional(),
    editor: z.unknown().optional(),
}).strict();

export type EntisiumConfig = z.infer<typeof configSchema>;

export function parseConfig(value: unknown): EntisiumConfig {
    const parsed = configSchema.safeParse(value);
    if (!parsed.success) {
        // Do not include Zod's received values, source snippets or arbitrary property names.
        const fields = parsed.error.issues.map((issue) => issue.path
            .map((part, index) => index > 0 && issue.path[0] === "providers" && index === 1 ? "<provider>" : String(part)).join("."));
        throw new Error(`Invalid config.yaml fields: ${[...new Set(fields)].join(", ") || "root"}.`);
    }
    return parsed.data;
}
