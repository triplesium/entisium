import { z } from "zod/v4";
import { modelSelectionSchema, type EntisiumConfig } from "@entisium/devkit/settings/config";
import { chatConnection, selectedProvider } from "@entisium/devkit/settings/providers";
import type { EditorModelSettings } from "../models/model-settings-store.js";

export const agentSettingsSchema = z.object({
    model: modelSelectionSchema.optional(),
    reasoning: z.enum(["off", "minimal", "low", "medium", "high", "xhigh", "max"]).default("low"),
}).strict();
export type AgentSettings = z.infer<typeof agentSettingsSchema>;
export function parseAgentSettings(value: unknown): AgentSettings {
    const result = agentSettingsSchema.safeParse(value ?? {});
    if (!result.success) throw new Error("Invalid config.yaml agent settings; check model and reasoning.");
    return result.data;
}

const modelSchema = z.object({
    id: z.string().min(1), name: z.string().min(1).optional(), reasoning: z.boolean().optional(),
    contextWindow: z.number().int().min(1024).max(10_000_000).optional(),
    maxTokens: z.number().int().min(256).max(10_000_000).optional(),
}).strict().refine((model) => model.maxTokens === undefined || model.contextWindow === undefined || model.maxTokens <= model.contextWindow);

/** Explicit projection: apiKey is never copied into the registry's browser-visible settings. */
export function modelSettingsFromConfig(config: EntisiumConfig): EditorModelSettings {
    const agent = parseAgentSettings(config.agent);
    const connections = { ...config.providers };
    if (agent.model && !Object.hasOwn(connections, agent.model.provider)) {
        Object.defineProperty(connections, agent.model.provider, { value: selectedProvider(config, agent.model.provider), enumerable: true });
    }
    const providers = Object.entries(connections).flatMap(([id, provider]) => {
        const connection = chatConnection(id, provider);
        if (!connection) return [];
        const parsed = z.array(modelSchema).safeParse(provider.models ?? []);
        if (!parsed.success) throw new Error("Invalid config.yaml provider model catalogue.");
        return [{
            id, name: provider.name ?? id, ...connection, type: provider.type,
            models: parsed.data,
        }];
    });
    if (agent.model && !providers.some((provider) => provider.id === agent.model!.provider)) {
        throw new Error("config.yaml agent.model requires a provider with a supported chat connection.");
    }
    return { version: 2, providers, ...(agent.model ? { active: { providerId: agent.model.provider, modelId: agent.model.id } } : {}) };
}
