import type { CredentialStore } from "@earendil-works/pi-ai";
import { ImageGenerationService } from "@entisium/devkit/image-generation/service";
import type { HostProjectService } from "@entisium/devkit/workspace/project-service";
import type { EntisiumConfig } from "@entisium/devkit/settings/config";
import { imageConnection, selectedProvider } from "@entisium/devkit/settings/providers";
import { ModelMetadataService } from "@entisium/devkit/models/service";

/** Shared Node host composition for the CLI and Editor server. */
export function createHostImageGeneration(project: HostProjectService, credentials: CredentialStore, config?: EntisiumConfig, metadata = new ModelMetadataService()) {
    const provider = config?.imageGeneration.model.provider ?? "openai";
    const settings = selectedProvider(config, provider);
    const connection = imageConnection(provider, settings);
    const type = settings.type;
    return new ImageGenerationService(project, {
        ...connection,
        model: process.env.ETS_IMAGE_MODEL?.trim() || process.env.ENTISIUM_IMAGE_MODEL?.trim() || config?.imageGeneration.model.id || "gpt-image-2",
        timeoutMs: config?.imageGeneration.timeoutMs,
        defaults: config?.imageGeneration.defaults,
        resolveMetadata: (model, apiKey, signal) => metadata.getModel({ providerId: provider, type, baseUrl: connection.baseUrl, apiKey }, model,
            { signal, includeSchema: type === "fal" }),
        resolveApiKey: async (signal) => {
            const key = type === "fal" ? process.env.FAL_KEY?.trim()
                : type === "openai" ? process.env.OPENAI_API_KEY?.trim()
                : type === "openrouter" ? process.env.OPENROUTER_API_KEY?.trim() : undefined;
            if (key) return key;
            const credential = await credentials.read(provider, { signal });
            return credential?.type === "api_key" ? credential.key : undefined;
        },
    });
}
