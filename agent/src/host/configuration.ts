import type { AuthOperationOptions, Credential, CredentialStore } from "@earendil-works/pi-ai";
import { YamlConfigStore, loadConfig } from "@entisium/devkit/settings/yaml-store";
import { chatConnection, type ProviderSettings } from "@entisium/devkit/settings/providers";
import { EncryptedCredentialStore } from "../models/credential-store.js";
import { FileEditorModelSettingsStore, type EditorModelSettings, type EditorModelSettingsStore } from "../models/model-settings-store.js";
import { modelSettingsFromConfig, parseAgentSettings } from "../settings/config.js";

export class YamlCredentialStore implements CredentialStore {
    constructor(private readonly config: YamlConfigStore) {}
    async read(providerId: string, options?: AuthOperationOptions): Promise<Credential | undefined> {
        options?.signal?.throwIfAborted();
        const config = await this.config.read();
        options?.signal?.throwIfAborted();
        const key = Object.hasOwn(config.providers, providerId) ? config.providers[providerId].apiKey : undefined;
        return key ? { type: "api_key", key } : undefined;
    }
    async list(options?: AuthOperationOptions) {
        options?.signal?.throwIfAborted();
        return Object.entries((await this.config.read()).providers)
            .filter(([, provider]) => Boolean(provider.apiKey)).map(([providerId]) => ({ providerId, type: "api_key" as const }));
    }
    async modify(providerId: string, edit: (current: Credential | undefined) => Promise<Credential | undefined>, options?: AuthOperationOptions): Promise<Credential | undefined> {
        let result: Credential | undefined;
        await this.config.update(async (config) => {
            options?.signal?.throwIfAborted();
            if (!Object.hasOwn(config.providers, providerId)) throw new Error("Configure the provider and its type before saving credentials.");
            const provider = config.providers[providerId];

            result = await edit(provider.apiKey ? { type: "api_key", key: provider.apiKey } : undefined);
            options?.signal?.throwIfAborted();
            if (result !== undefined) {
                if (result.type !== "api_key") throw new Error("config.yaml supports API key credentials only.");
                provider.apiKey = result.key;
                Object.defineProperty(config.providers, providerId, { value: provider, enumerable: true, writable: true, configurable: true });
            }
        });
        return result;
    }
    async delete(providerId: string, options?: AuthOperationOptions): Promise<void> {
        await this.config.update((config) => {
            options?.signal?.throwIfAborted();
            if (Object.hasOwn(config.providers, providerId)) delete config.providers[providerId].apiKey;
        });
    }
}

export class YamlModelSettingsStore implements EditorModelSettingsStore {
    constructor(private readonly config: YamlConfigStore) {}
    async read(): Promise<EditorModelSettings> { return modelSettingsFromConfig(await this.config.read()); }
    async write(settings: EditorModelSettings): Promise<void> {
        await this.config.update((config) => {
            const retained = new Set(settings.providers.map((provider) => provider.id));
            for (const [id, provider] of Object.entries(config.providers)) {
                if (retained.has(id) || !chatConnection(id, provider)) continue;
                if (config.imageGeneration.model.provider === id || provider.images) {
                    throw new Error("This provider is shared with image generation; remove or reassign its image connection before deleting it.");
                }
                const references = ["llm", "decisions"] as const;
                for (const section of references) {
                    for (const [alias, model] of Object.entries(config[section]?.models ?? {})) {
                        if (model.provider === id) {
                            throw new Error(`This provider is used by ${section}.models.${alias}; reassign that model before deleting it.`);
                        }
                    }
                }
                delete config.providers[id];
            }
            for (const { id, name, baseUrl, api, models, type } of settings.providers) {
                const previous = Object.hasOwn(config.providers, id) ? config.providers[id] : undefined;
                const provider: ProviderSettings = { ...previous, name,
                    type: previous?.type ?? type ?? "openai-compatible", chat: { api, baseUrl } };
                if (models.length) provider.models = models;
                else delete provider.models;
                Object.defineProperty(config.providers, id, { value: provider, enumerable: true, writable: true, configurable: true });
            }
            config.agent = { ...parseAgentSettings(config.agent),
                model: settings.active ? { provider: settings.active.providerId, id: settings.active.modelId } : undefined };
            modelSettingsFromConfig(config);
        });
    }
}

export async function loadHostConfiguration(path?: string, projectDirectory?: string) {
    const { store, config } = await loadConfig(path, projectDirectory);
    if (!config) {
        return { credentials: new EncryptedCredentialStore() as CredentialStore,
            modelSettingsStore: new FileEditorModelSettingsStore() as EditorModelSettingsStore,
            config: undefined, store, agent: parseAgentSettings(undefined) };
    }
    modelSettingsFromConfig(config);
    return { credentials: new YamlCredentialStore(store) as CredentialStore,
        modelSettingsStore: new YamlModelSettingsStore(store) as EditorModelSettingsStore,
        config, store, agent: parseAgentSettings(config.agent) };
}
