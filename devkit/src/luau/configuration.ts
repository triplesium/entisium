import { loadConfig } from "../settings/yaml-store.js";
import { chatConnection, selectedProvider } from "../settings/providers.js";

export interface NativeModelBinding {
    id: string; protocol: "jev" | "responses" | "chat-completions";
    connection: string; timeout: number; max_tokens?: number;
}
export interface NativeHostConfiguration {
    version: 1;
    connections: Record<string, { base_url: string; headers: { name: string; value: string }[]; error?: string }>;
    generation: Record<string, NativeModelBinding>;
    decisions: Record<string, NativeModelBinding>;
    tape?: { mode: "record" | "replay"; entries?: unknown[] };
}
export function emptyHostConfiguration(): NativeHostConfiguration {
    return { version: 1, connections: {}, generation: {}, decisions: {} };
}
export async function configuredLuauHost(projectDirectory: string,
    resolveCredential?: (provider: string, signal: AbortSignal) => Promise<string | undefined>,
    signal: AbortSignal = new AbortController().signal): Promise<NativeHostConfiguration> {
    signal.throwIfAborted();
    const { config } = await loadConfig(undefined, projectDirectory);
    const result = emptyHostConfiguration();
    if (!config) return result;
    const credentials = new Map<string, string | undefined>();
    for (const kind of ["generation", "decisions"] as const) {
        const selection = kind === "generation" ? config.llm?.models ?? {}
            : config.decisions?.models ?? { "fast-decision": { provider: "typesafe", id: "jev-latest" } };
        for (const [alias, model] of Object.entries(selection)) {
            const provider = selectedProvider(config, model.provider);
            let baseUrl: string, protocol: NativeModelBinding["protocol"];
            if (kind === "generation") {
                const connection = chatConnection(model.provider, provider);
                if (!connection) throw new Error("LLM provider has no chat connection.");
                baseUrl = connection.baseUrl; protocol = connection.api;
            } else {
                const url = provider.decisions?.baseUrl ?? (provider.type === "typesafe" ? "https://api.typesafe.ai/v1" : undefined);
                if (!url) throw new Error("A custom decision provider requires decisions.baseUrl.");
                baseUrl = url; protocol = "jev";
            }
            if (!credentials.has(model.provider)) {
                const key = provider.apiKey ?? await resolveCredential?.(model.provider, signal)
                    ?? (provider.type === "typesafe" ? process.env.TYPESAFE_API_KEY
                        : provider.type === "openai" ? process.env.OPENAI_API_KEY
                        : provider.type === "openrouter" ? process.env.OPENROUTER_API_KEY : undefined);
                signal.throwIfAborted();
                if (key !== undefined && (!key || key.length > 4096 || !/^[\x21-\x7e]+$/.test(key))) throw new Error("Invalid model provider credential.");
                credentials.set(model.provider, key);
            }
            const key = credentials.get(model.provider);
            const connection = `${kind}:${model.provider}`;
            Object.defineProperty(result.connections, connection, { enumerable: true, configurable: true, value: {
                base_url: baseUrl, headers: key ? [{ name: "Authorization", value: `Bearer ${key}` }] : [],
                ...(key ? {} : { error: "Model provider credential is missing" }),
            } });
            Object.defineProperty(result[kind], alias, { enumerable: true, configurable: true, value: {
                id: model.id, protocol, connection,
                timeout: (kind === "generation" ? config.llm?.timeoutMs ?? 30_000 : config.decisions?.timeoutMs ?? 30_000) / 1000,
                ...(kind === "generation" ? { max_tokens: config.llm?.maxTokens ?? 4096 } : {}),
            } });
        }
    }
    return result;
}
export function encodeHostConfiguration(config: NativeHostConfiguration): string {
    const text = JSON.stringify(config);
    if (Buffer.byteLength(text) > 32 * 1024 * 1024) throw new Error("Luau host configuration exceeds 32 MiB.");
    return text + "\n";
}
