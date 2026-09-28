import {
    createModels,
    createProvider,
    type Api,
    type Context,
    type CredentialStore,
    type Model,
    type SimpleStreamOptions,
} from "@earendil-works/pi-ai";
import { openAICompletionsApi } from "@earendil-works/pi-ai/api/openai-completions.lazy";
import { openAIResponsesApi } from "@earendil-works/pi-ai/api/openai-responses.lazy";
import { catalogChatMetadata, chatRuntimeSettings, fallbackChatMetadata, metadataTarget, resolveChatMetadata } from "./model-metadata.js";
import { ModelMetadataService } from "@entisium/devkit/models/service";
import type { ResolvedModelMetadata } from "@entisium/devkit/models/metadata";
import { createHash } from "node:crypto";
import {
    type EditorModelSettings,
    type EditorModelSettingsStore,
    type OpenAICompatibleModelSettings,
    type OpenAICompatibleProviderSettings,
} from "./model-settings-store.js";

export interface ModelProviderSummary {
    id: string;
    name: string;
    baseUrl: string;
    api: OpenAICompatibleProviderSettings["api"];
    configured: boolean;
    models: Model<Api>[];
    metadata?: Record<string, ResolvedModelMetadata>;
}

export interface ModelRegistrySnapshot {
    active: { providerId: string; modelId: string };
    providers: ModelProviderSummary[];
}

export interface ConfigureModelInput {
    providerId: string;
    modelId: string;
    apiKey?: string;
    provider?: OpenAICompatibleProviderSettings;
}

export interface ConfigureProviderInput {
    provider: Omit<OpenAICompatibleProviderSettings, "models">;
    apiKey?: string;
}

export interface ConfigureRegistryModelInput {
    providerId: string;
    previousModelId?: string;
    model: OpenAICompatibleModelSettings;
}

const zeroCost = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 };

function normalizedUrl(value: string): string {
    const source = value.trim().replace(/\/+$/, "");
    let url: URL;
    try {
        url = new URL(source);
    } catch {
        throw new Error("API Base URL must be a valid URL.");
    }
    if (url.protocol !== "https:" && url.protocol !== "http:") {
        throw new Error("API Base URL must use HTTP or HTTPS.");
    }
    if (url.username || url.password || url.search || url.hash) {
        throw new Error("API Base URL cannot contain credentials, a query, or a fragment.");
    }
    return source;
}

function nonEmpty(value: string, label: string, maximum = 160): string {
    const next = value.trim();
    if (!next) throw new Error(`${label} is required.`);
    if (next.length > maximum) throw new Error(`${label} is too long.`);
    return next;
}

function identifier(value: string, label: string): string {
    const next = nonEmpty(value, label, 120);
    if (!/^[A-Za-z0-9][A-Za-z0-9._:/-]*$/.test(next)) {
        throw new Error(`${label} may only contain letters, numbers, dots, dashes, underscores, slashes, and colons.`);
    }
    return next;
}

function validApiKey(value: string): boolean {
    return value.length >= 8 && value.length <= 4_096 && /^[\x21-\x7e]+$/.test(value);
}

function integer(value: number, label: string, minimum: number, maximum: number): number {
    if (!Number.isInteger(value) || value < minimum || value > maximum) {
        throw new Error(`${label} must be between ${minimum} and ${maximum}.`);
    }
    return value;
}

function normalizeModel(input: OpenAICompatibleModelSettings): OpenAICompatibleModelSettings {
    const contextWindow = input.contextWindow === undefined ? undefined : integer(input.contextWindow, "Context window", 1_024, 10_000_000);
    return {
        id: identifier(input.id, "Model ID"),
        ...(input.name === undefined ? {} : { name: nonEmpty(input.name, "Model name") }),
        ...(input.reasoning === undefined ? {} : { reasoning: Boolean(input.reasoning) }),
        ...(contextWindow === undefined ? {} : { contextWindow }),
        ...(input.maxTokens === undefined ? {} : { maxTokens: integer(input.maxTokens, "Maximum output tokens", 256, contextWindow ?? 10_000_000) }),
    };
}

function normalizeProvider(
    input: OpenAICompatibleProviderSettings,
): OpenAICompatibleProviderSettings {
    if (!Array.isArray(input.models)) throw new Error("Provider models must be a list.");
    const models = input.models.map(normalizeModel);
    if (new Set(models.map((model) => model.id)).size !== models.length) {
        throw new Error("Model IDs must be unique within a provider.");
    }
    return {
        id: identifier(input.id, "Provider ID"),
        name: nonEmpty(input.name, "Provider name"),
        baseUrl: normalizedUrl(input.baseUrl),
        api: input.api === "chat-completions" ? "chat-completions" : "responses",
        ...(input.type ? { type: input.type } : {}),
        models,
    };
}

function runtimeModel(settings: OpenAICompatibleProviderSettings, model: Required<OpenAICompatibleModelSettings>): Model<Api> {
    const api = settings.api === "responses" ? "openai-responses" : "openai-completions";
    return {
        id: model.id,
        name: model.name,
        api,
        provider: settings.id,
        baseUrl: settings.baseUrl,
        reasoning: model.reasoning,
        input: ["text"],
        cost: zeroCost,
        contextWindow: model.contextWindow,
        maxTokens: model.maxTokens,
        ...(settings.api === "responses"
            ? {
                  thinkingLevelMap: {
                      off: "none",
                      minimal: "minimal",
                      low: "low",
                      medium: "medium",
                      high: "high",
                      xhigh: "xhigh",
                      max: "max",
                  },
                  compat: {
                      supportsDeveloperRole: false,
                      supportsLongCacheRetention: false,
                  },
              }
            : {}),
    };
}

function runtimeProvider(settings: OpenAICompatibleProviderSettings, resolvedModels?: readonly Model<Api>[]) {
    const models = resolvedModels ?? settings.models.map((model) => runtimeModel(settings, chatRuntimeSettings(fallbackChatMetadata(settings, model.id))));
    return createProvider({
        id: settings.id,
        name: settings.name,
        baseUrl: settings.baseUrl,
        auth: {
            apiKey: {
                name: `${settings.name} API key`,
                resolve: async ({ credential, signal }) => {
                    signal.throwIfAborted();
                    return credential?.key
                        ? { auth: { apiKey: credential.key }, source: "stored credential" }
                        : undefined;
                },
            },
        },
        models,
        api: settings.api === "responses" ? openAIResponsesApi() : openAICompletionsApi(),
    });
}

export class HostModelRegistry {
    private readonly models;
    private readonly readyPromise: Promise<void>;
    private readonly warnedCredentialProviders = new Set<string>();
    private settings!: EditorModelSettings;
    private readonly metadata = new Map<string, ResolvedModelMetadata>();
    private readonly credentialScopes = new Map<string, string>();

    constructor(
        private readonly credentials: CredentialStore,
        private readonly settingsStore: EditorModelSettingsStore,
        private readonly metadataService = new ModelMetadataService(),
    ) {
        this.models = createModels({ credentials });
        this.readyPromise = this.initialize();
    }

    async snapshot(): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const active = this.resolveActive();
        const providers = await Promise.all(
            this.settings.providers.map(async (settings): Promise<ModelProviderSummary> => {
                const provider = this.models.getProvider(settings.id);
                if (!provider) throw new Error(`Provider ${settings.id} is unavailable.`);
                return {
                    id: settings.id,
                    name: settings.name,
                    baseUrl: settings.baseUrl,
                    api: settings.api,
                    configured: await this.isConfigured(settings.id),
                    models: provider.getModels().map((model) => structuredClone(model)),
                    metadata: Object.fromEntries(provider.getModels().flatMap((model) => {
                        const metadata = this.metadata.get(JSON.stringify([settings.id, model.id]));
                        return metadata ? [[model.id, structuredClone(metadata)]] : [];
                    })),
                };
            }),
        );
        return { active, providers };
    }

    async configure(input: ConfigureModelInput): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const providerId = identifier(input.providerId, "Provider ID");
        const modelId = identifier(input.modelId, "Model ID");
        const nextSettings = structuredClone(this.settings);

        if (input.provider) {
            const provider = normalizeProvider(input.provider);
            if (provider.id !== providerId) {
                throw new Error("The configured provider must match the selected Provider ID.");
            }
            const existingIndex = nextSettings.providers.findIndex((candidate) => candidate.id === provider.id);
            if (existingIndex >= 0) nextSettings.providers[existingIndex] = provider;
            else nextSettings.providers.push(provider);
        }

        if (!nextSettings.providers.some((provider) => provider.id === providerId)) throw new Error("Unknown model provider.");
        const apiKey = input.apiKey?.trim();
        if (apiKey !== undefined) {
            if (!validApiKey(apiKey)) {
                throw new Error(
                    "API key must contain between 8 and 4096 printable ASCII characters without spaces.",
                );
            }
        }
        nextSettings.active = { providerId, modelId };
        await this.settingsStore.write(nextSettings);
        this.settings = nextSettings;
        if (apiKey !== undefined) await this.credentials.modify(providerId, async () => ({ type: "api_key", key: apiKey }));
        if (input.provider || input.apiKey !== undefined) this.replaceProvider(nextSettings.providers.find((provider) => provider.id === providerId)!);
        await this.getModel(providerId, modelId);
        return this.snapshot();
    }

    async configureProvider(input: ConfigureProviderInput): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const providerInput = input.provider;
        const existing = this.settings.providers.find((provider) => provider.id === providerInput.id);
        const provider = normalizeProvider({
            ...providerInput,
            type: providerInput.type ?? existing?.type,
            models: existing?.models ?? [],
        });
        const nextSettings = structuredClone(this.settings);
        const existingIndex = nextSettings.providers.findIndex((candidate) => candidate.id === provider.id);
        if (existingIndex >= 0) nextSettings.providers[existingIndex] = provider;
        else nextSettings.providers.push(provider);

        const apiKey = input.apiKey?.trim();
        if (apiKey !== undefined) {
            if (!validApiKey(apiKey)) {
                throw new Error(
                    "API key must contain between 8 and 4096 printable ASCII characters without spaces.",
                );
            }
        }
        await this.settingsStore.write(nextSettings);
        this.settings = nextSettings;
        if (apiKey !== undefined) await this.credentials.modify(provider.id, async () => ({ type: "api_key", key: apiKey }));
        this.replaceProvider(provider);
        if (this.settings.active?.providerId === provider.id) this.registerSelectedModel(provider.id, this.settings.active.modelId);
        return this.snapshot();
    }

    async configureRegistryModel(input: ConfigureRegistryModelInput): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const providerId = identifier(input.providerId, "Provider ID");
        const model = normalizeModel(input.model);
        const previousModelId = input.previousModelId
            ? identifier(input.previousModelId, "Previous Model ID")
            : undefined;
        const nextSettings = structuredClone(this.settings);
        const provider = nextSettings.providers.find((candidate) => candidate.id === providerId);
        if (!provider) throw new Error("Unknown model provider.");

        const existingIndex = previousModelId
            ? provider.models.findIndex((candidate) => candidate.id === previousModelId)
            : -1;
        if (previousModelId && existingIndex < 0 && !this.models.getModel(providerId, previousModelId)) throw new Error("The model to edit is unavailable.");
        if (
            provider.models.some(
                (candidate, index) => candidate.id === model.id && index !== existingIndex,
            )
        ) {
            throw new Error("Model IDs must be unique within a provider.");
        }
        if (existingIndex >= 0) provider.models[existingIndex] = model;
        else provider.models.push(model);

        if (
            previousModelId &&
            nextSettings.active?.providerId === providerId &&
            nextSettings.active.modelId === previousModelId
        ) {
            nextSettings.active.modelId = model.id;
        }
        nextSettings.active ??= { providerId, modelId: model.id };
        await this.settingsStore.write(nextSettings);
        this.settings = nextSettings;
        this.replaceProvider(provider);
        return this.snapshot();
    }

    async deleteModel(providerIdValue: string, modelIdValue: string): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const providerId = identifier(providerIdValue, "Provider ID");
        const modelId = identifier(modelIdValue, "Model ID");
        const nextSettings = structuredClone(this.settings);
        const providerIndex = nextSettings.providers.findIndex((provider) => provider.id === providerId);
        const provider = nextSettings.providers[providerIndex];
        if (!provider || !this.models.getModel(providerId, modelId)) {
            throw new Error("The model to delete is unavailable.");
        }

        const modelCount = nextSettings.providers.reduce(
            (total, candidate) => total + (this.models.getProvider(candidate.id)?.getModels().length ?? 0),
            0,
        );
        if (modelCount === 1) {
            throw new Error("At least one model must remain configured.");
        }
        provider.models = provider.models.filter((model) => model.id !== modelId);
        const remaining = this.settings.providers.flatMap((candidate) =>
            (this.models.getProvider(candidate.id)?.getModels() ?? []).filter((model) => candidate.id !== providerId || model.id !== modelId));
        if (nextSettings.active?.providerId === providerId && nextSettings.active.modelId === modelId) {
            nextSettings.active = {
                providerId: remaining[0].provider,
                modelId: remaining[0].id,
            };
        }
        await this.settingsStore.write(nextSettings);
        this.settings = nextSettings;
        this.models.setProvider(runtimeProvider({ ...provider, models: remaining.filter((model) => model.provider === providerId) }));
        return this.snapshot();
    }

    async deleteProvider(providerIdValue: string): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const providerId = identifier(providerIdValue, "Provider ID");
        const nextSettings = structuredClone(this.settings);
        const provider = nextSettings.providers.find((candidate) => candidate.id === providerId);
        if (!provider) throw new Error("Unknown model provider.");
        const remainingModelCount = nextSettings.providers.reduce(
            (total, candidate) => total + (candidate.id === providerId ? 0 : this.models.getProvider(candidate.id)?.getModels().length ?? 0),
            0,
        );
        if (remainingModelCount === 0) throw new Error("At least one model must remain configured.");

        nextSettings.providers = nextSettings.providers.filter((candidate) => candidate.id !== providerId);
        if (nextSettings.active?.providerId === providerId) {
            const fallbackModel = nextSettings.providers.flatMap((candidate) => this.models.getProvider(candidate.id)?.getModels() ?? [])[0];
            nextSettings.active = {
                providerId: fallbackModel.provider,
                modelId: fallbackModel.id,
            };
        }
        await this.settingsStore.write(nextSettings);
        this.settings = nextSettings;
        for (const model of this.models.getProvider(providerId)?.getModels() ?? []) this.metadata.delete(JSON.stringify([providerId, model.id]));
        this.models.deleteProvider(providerId);
        await this.credentials.delete(providerId);
        this.credentialScopes.delete(providerId);
        return this.snapshot();
    }

    async deleteCredential(providerId: string): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        if (!this.models.getProvider(providerId)) throw new Error("Unknown model provider.");
        await this.credentials.delete(providerId);
        const provider = this.settings.providers.find((candidate) => candidate.id === providerId);
        if (provider) this.replaceProvider(provider);
        return this.snapshot();
    }

    async activeModel(signal?: AbortSignal): Promise<{ provider: ModelProviderSummary; model: Model<Api> }> {
        await this.readyPromise;
        const selected = this.resolveActive();
        await this.getModel(selected.providerId, selected.modelId, signal);
        const snapshot = await this.snapshot();
        const provider = snapshot.providers.find((candidate) => candidate.id === snapshot.active.providerId);
        const model = provider?.models.find((candidate) => candidate.id === snapshot.active.modelId);
        if (!provider || !model) throw new Error("The active Editor model is unavailable.");
        return { provider, model };
    }

    async getModel(providerId: string, modelId: string, signal?: AbortSignal): Promise<Model<Api> | undefined> {
        await this.readyPromise;
        providerId = identifier(providerId, "Provider ID");
        modelId = identifier(modelId, "Model ID");
        const provider = this.settings.providers.find((candidate) => candidate.id === providerId);
        if (!provider) return undefined;
        const credential = await this.credentials.read(providerId, { signal }).catch(() => undefined);
        signal?.throwIfAborted();
        const key = credential?.type === "api_key" ? credential.key : undefined;
        const scope = this.metadataScope(provider, key);
        const metadata = await resolveChatMetadata(this.metadataService, provider, modelId, key, signal);
        return structuredClone(this.publishModel(this.credentialScopes.get(providerId) === scope ? provider : { ...provider }, modelId, metadata));
    }

    async refreshModels(providerId: string, force = false): Promise<ModelRegistrySnapshot> {
        await this.readyPromise;
        const provider = this.settings.providers.find((candidate) => candidate.id === providerId);
        if (!provider) throw new Error("Unknown model provider.");
        const credential = await this.credentials.read(providerId).catch(() => undefined);
        const target = metadataTarget(provider, credential?.type === "api_key" ? credential.key : undefined);
        const scope = this.metadataScope(provider, target.apiKey);
        const catalog = await this.metadataService.listModels(target, { force });
        if (this.settings.providers.find((candidate) => candidate.id === providerId) !== provider || this.credentialScopes.get(providerId) !== scope) return this.snapshot();
        // Only chat-capable catalog entries appear in the Agent selector. Direct IDs remain allowed.
        const models = new Map((this.models.getProvider(providerId)?.getModels() ?? []).map((model) => [model.id, model]));
        for (const model of catalog.models) {
            if (model.outputModalities && !model.outputModalities.includes("text")) continue;
            if (model.id.length > 120 || !/^[A-Za-z0-9][A-Za-z0-9._:/-]*$/.test(model.id)) continue;
            const metadata = { ...catalogChatMetadata(provider, model, catalog.fetchedAt), stale: catalog.stale, status: catalog.status };
            models.set(model.id, this.publishModel(provider, model.id, metadata, false));
        }
        this.models.setProvider(runtimeProvider(provider, [...models.values()]));
        return this.snapshot();
    }

    streamSimple(model: Model<Api>, context: Context, options?: SimpleStreamOptions) {
        return this.models.streamSimple(model, context, options);
    }

    private async initialize(): Promise<void> {
        const settings = await this.settingsStore.read();
        settings.providers = settings.providers.map(normalizeProvider);
        if (new Set(settings.providers.map((provider) => provider.id)).size !== settings.providers.length) {
            throw new Error("Provider IDs must be unique.");
        }
        this.settings = settings;
        for (const provider of settings.providers) this.models.setProvider(runtimeProvider(provider));
        if (settings.active) this.registerSelectedModel(settings.active.providerId, settings.active.modelId);
    }

    private registerSelectedModel(providerId: string, modelId: string): void {
        if (this.models.getModel(providerId, modelId)) return;
        const provider = this.settings.providers.find((candidate) => candidate.id === providerId);
        if (!provider) return;
        this.publishModel(provider, modelId, fallbackChatMetadata(provider, modelId));
    }

    private replaceProvider(provider: OpenAICompatibleProviderSettings): void {
        for (const model of this.models.getProvider(provider.id)?.getModels() ?? []) this.metadata.delete(JSON.stringify([provider.id, model.id]));
        this.models.setProvider(runtimeProvider(provider));
    }

    private metadataScope(provider: OpenAICompatibleProviderSettings, apiKey?: string): string {
        const scope = createHash("sha256").update(JSON.stringify(metadataTarget(provider, apiKey))).digest("hex");
        if (this.settings.providers.find((candidate) => candidate.id === provider.id) !== provider) return scope;
        const previous = this.credentialScopes.get(provider.id);
        if (previous && previous !== scope) this.replaceProvider(provider);
        this.credentialScopes.set(provider.id, scope);
        return scope;
    }

    private publishModel(provider: OpenAICompatibleProviderSettings, id: string, metadata: ResolvedModelMetadata, publish = true): Model<Api> {
        const operating = chatRuntimeSettings(metadata);
        const model = runtimeModel(provider, operating);
        const input = metadata.model.inputModalities?.filter((modality): modality is "text" | "image" => modality === "text" || modality === "image");
        if (input?.length) model.input = input;
        if (this.settings.providers.find((candidate) => candidate.id === provider.id) === provider) {
            this.metadata.set(JSON.stringify([provider.id, id]), structuredClone(metadata));
            if (publish) {
                const others = (this.models.getProvider(provider.id)?.getModels() ?? []).filter((candidate) => candidate.id !== id);
                this.models.setProvider(runtimeProvider(provider, [...others, model]));
            }
        }
        return model;
    }

    private async isConfigured(providerId: string): Promise<boolean> {
        try {
            const credential = await this.credentials.read(providerId);
            if (
                credential?.type === "api_key" &&
                (typeof credential.key !== "string" || !validApiKey(credential.key))
            ) {
                return false;
            }
            return Boolean(await this.models.checkAuth(providerId));
        } catch {
            if (!this.warnedCredentialProviders.has(providerId)) {
                this.warnedCredentialProviders.add(providerId);
                console.warn(
                    `[entisium editor] ${providerId} credentials are unavailable; treating the provider as unconfigured.`,
                );
            }
            return false;
        }
    }

    private resolveActive(): { providerId: string; modelId: string } {
        const configuredModel = process.env.ETS_EDITOR_MODEL?.trim();
        if (configuredModel) {
            const providerId = this.settings.active?.providerId ?? this.settings.providers[0]?.id;
            if (providerId) {
                this.registerSelectedModel(providerId, identifier(configuredModel, "Model ID"));
                return { providerId, modelId: configuredModel };
            }
        }
        const active = this.settings.active;
        if (active) this.registerSelectedModel(active.providerId, active.modelId);
        if (active && this.models.getModel(active.providerId, active.modelId)) return { ...active };
        const provider = this.settings.providers.find((candidate) => candidate.models.length > 0);
        const model = provider?.models[0];
        if (!provider || !model) throw new Error("No OpenAI-compatible models are registered.");
        return { providerId: provider.id, modelId: model.id };
    }
}
