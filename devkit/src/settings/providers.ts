import type { EntisiumConfig } from "./config.js";

export type ProviderSettings = EntisiumConfig["providers"][string];
export type ProviderType = ProviderSettings["type"];

export function chatConnection(id: string, settings: ProviderSettings) {
    const type = settings.type;
    if (type === "fal" && !settings.chat) return undefined;
    const baseUrl = settings.chat?.baseUrl ?? (type === "openai" ? "https://api.openai.com/v1"
        : type === "openrouter" ? "https://openrouter.ai/api/v1" : undefined);
    // A custom connection can serve only images.
    if (!baseUrl && !settings.chat) return undefined;
    if (!baseUrl) throw new Error("The chat provider requires chat.baseUrl in config.yaml.");
    return { baseUrl, api: settings.chat?.api ?? (type === "openrouter" ? "chat-completions" : "responses") };
}

export function imageConnection(id: string, settings: ProviderSettings) {
    const type = settings.type;
    const api = settings.images?.api ?? (type === "openrouter" ? "openrouter-images" : type === "fal" ? "fal-images" : "openai-images");
    const baseUrl = settings.images?.baseUrl ?? (type === "openai" ? "https://api.openai.com/v1"
        : type === "openrouter" ? "https://openrouter.ai/api/v1" : undefined);
    if (api === "fal-images") {
        if (settings.images?.baseUrl) throw new Error("fal-images uses official endpoints and does not accept images.baseUrl.");
        return { api, baseUrl: undefined };
    }
    if (!baseUrl) throw new Error("The image provider requires baseUrl in providers.<id>.images.");
    return { api, baseUrl };
}

/** Built-ins may be used without a provider block (for environment/stored credentials). */
export function selectedProvider(config: EntisiumConfig | undefined, id: string): ProviderSettings {
    if (config && Object.hasOwn(config.providers, id)) return config.providers[id];
    if (id === "openai" || id === "openrouter" || id === "fal" || id === "typesafe") return { type: id };
    throw new Error("The selected provider is not configured in config.yaml.");
}
