import { RuntimeSession } from "@entisium/devkit/runtime/session";
import { ModelMetadataService } from "@entisium/devkit/models/service";
import { createHostImageGeneration } from "@entisium/agent/host/image-generation";
import type { EntisiumConfig } from "@entisium/devkit/settings/config";
import { NativeRuntime } from "@entisium/devkit/runtime/native-runtime";
import { parseAgentSettings } from "@entisium/agent/settings/config";
import { imageGenerationSchema, type ImageGenerationInvoker } from "@entisium/devkit/contracts/image-generation";
import { SpriteAnimationService } from "@entisium/devkit/sprite-animation/service";
import { createHostSpriteVideo } from "@entisium/agent/host/sprite-video";
import { spriteAnimationSchema } from "@entisium/devkit/contracts/sprite-animation";
import { randomBytes } from "node:crypto";
import { createReadStream } from "node:fs";
import { access, readFile, stat } from "node:fs/promises";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { extname, join, resolve, sep } from "node:path";
import type { AddressInfo } from "node:net";
import type { Duplex } from "node:stream";
import {
    type Context,
    type CredentialStore,
    type Model,
    type SimpleStreamOptions,
} from "@earendil-works/pi-ai";
import {
    HostModelRegistry,
    type ConfigureModelInput,
    type ConfigureProviderInput,
    type ConfigureRegistryModelInput,
} from "@entisium/agent/models/model-registry";
import {
    MemoryEditorModelSettingsStore,
    type EditorModelSettingsStore,
} from "@entisium/agent/models/model-settings-store";
import {
    MemoryEditorSettingsStore,
    type EditorSettings,
    type EditorSettingsStore,
} from "./editor-settings-store.js";
import {
    HostProjectService,
    ProjectPickerCancelledError,
    type ProjectDirectoryPicker,
} from "@entisium/devkit/workspace/project-service";
import { toProxyEvent } from "@entisium/agent/models/proxy-events";
import {
    EditorCommandRelay,
    type EditorCommandResponse,
} from "./editor-command-relay.js";
import { handleMcpRequest } from "./mcp-server.js";
import { LuauLspGateway } from "./luau-lsp-session.js";

const maximumJsonBodyBytes = 4 * 1024 * 1024;
interface HostOptions {
    credentials: CredentialStore;
    editorSettingsStore?: EditorSettingsStore;
    modelSettingsStore?: EditorModelSettingsStore;
    modelMetadataService?: ModelMetadataService;
    distDirectory: string;
    runtimeDirectory: string;
    host?: string;
    port?: number;
    allowedOrigins?: readonly string[];
    projectDirectory?: string;
    pickProjectDirectory?: ProjectDirectoryPicker;
    projectService?: HostProjectService;
    commandRelay?: EditorCommandRelay;
    nativeSession?: RuntimeSession;
    generateImage?: ImageGenerationInvoker;
    config?: EntisiumConfig;
    runtimeExecutable?: string;
    luauLspExecutable?: string;
    luauLspArguments?: readonly string[];
    luauDefinitionsIndex?: string;
}

interface ProxyRequest {
    model?: { provider?: unknown; id?: unknown };
    context?: unknown;
    options?: unknown;
}

interface ProfileSymbolManifest {
    schema: string;
    module_id: string;
    kind: string;
    symbols: Record<string, unknown>;
}

const contentTypes: Record<string, string> = {
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".ttf": "font/ttf",
    ".wasm": "application/wasm",
};

function json(response: ServerResponse, status: number, value: unknown): void {
    response.writeHead(status, { "Content-Type": "application/json; charset=utf-8" });
    response.end(JSON.stringify(value));
}

function errorMessage(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

async function readJson(request: IncomingMessage, maximumBytes = maximumJsonBodyBytes): Promise<unknown> {
    const chunks: Buffer[] = [];
    let size = 0;
    for await (const chunk of request) {
        const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
        size += buffer.length;
        if (size > maximumBytes) throw new Error("Request body is too large.");
        chunks.push(buffer);
    }
    const source = Buffer.concat(chunks).toString("utf8");
    if (!source) return {};
    return JSON.parse(source);
}

function asObject(value: unknown): Record<string, unknown> {
    if (!value || typeof value !== "object" || Array.isArray(value)) {
        throw new Error("Expected a JSON object.");
    }
    return value as Record<string, unknown>;
}

function sanitizeContext(value: unknown): Context {
    const context = asObject(value);
    if (!Array.isArray(context.messages)) throw new Error("Agent context requires messages.");
    if (context.systemPrompt !== undefined && typeof context.systemPrompt !== "string") {
        throw new Error("Agent system prompt must be a string.");
    }
    if (context.tools !== undefined && !Array.isArray(context.tools)) {
        throw new Error("Agent tools must be a list.");
    }
    return {
        systemPrompt: context.systemPrompt as string | undefined,
        messages: context.messages as Context["messages"],
        tools: context.tools as Context["tools"],
    };
}

function finiteNumber(value: unknown, minimum: number, maximum: number): number | undefined {
    if (typeof value !== "number" || !Number.isFinite(value)) return undefined;
    return Math.max(minimum, Math.min(maximum, value));
}

function sanitizeOptions(value: unknown, model: Model<any>, signal: AbortSignal): SimpleStreamOptions {
    const options = value && typeof value === "object" && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : {};
    const reasoning =
        typeof options.reasoning === "string" &&
        ["off", "minimal", "low", "medium", "high", "xhigh", "max"].includes(options.reasoning)
            ? (options.reasoning as SimpleStreamOptions["reasoning"])
            : undefined;
    const cacheRetention =
        options.cacheRetention === "none" ||
        options.cacheRetention === "short" ||
        options.cacheRetention === "long"
            ? options.cacheRetention
            : undefined;
    return {
        signal,
        temperature: finiteNumber(options.temperature, 0, 2),
        maxTokens: finiteNumber(options.maxTokens, 1, model.maxTokens),
        reasoning,
        cacheRetention,
        sessionId:
            typeof options.sessionId === "string" && options.sessionId.length <= 256
                ? options.sessionId
                : undefined,
        maxRetryDelayMs: finiteNumber(options.maxRetryDelayMs, 0, 60_000),
    };
}

function bearerToken(request: IncomingMessage): string | undefined {
    const authorization = request.headers.authorization;
    return authorization?.startsWith("Bearer ") ? authorization.slice(7) : undefined;
}

function isLoopbackRequest(request: IncomingMessage): boolean {
    const address = request.socket.remoteAddress;
    return Boolean(
        address === "::1" ||
            address === "127.0.0.1" ||
            address?.startsWith("127.") ||
            address?.startsWith("::ffff:127."),
    );
}

function modelForClient(model: Model<any>): Model<any> {
    return structuredClone(model);
}

export function createEditorHost(options: HostOptions): {
    server: Server;
    token: string;
    listen(): Promise<{ host: string; port: number }>;
    stopRuntime(): Promise<void>;
} {
    const host = options.host ?? "127.0.0.1";
    const port = options.port ?? 3100;
    const token = randomBytes(32).toString("base64url");
    const allowedOrigins = new Set([
        `http://${host}:${port}`,
        `http://localhost:${port}`,
        "http://127.0.0.1:5173",
        "http://localhost:5173",
        ...(options.allowedOrigins ?? []),
    ]);
    const profileSymbolManifests = new Map<
        string,
        Promise<ProfileSymbolManifest>
    >();

    const loadProfileSymbolManifest = async (
        digest: string,
    ): Promise<ProfileSymbolManifest> => {
        let pending = profileSymbolManifests.get(digest);
        if (!pending) {
            const file = resolve(
                options.runtimeDirectory,
                "profile-symbols",
                `${digest}.json`,
            );
            pending = readFile(file, "utf8").then(
                (source) => JSON.parse(source) as ProfileSymbolManifest,
            );
            profileSymbolManifests.set(digest, pending);
        }
        try {
            return await pending;
        } catch (error) {
            profileSymbolManifests.delete(digest);
            throw error;
        }
    };
    const metadata = options.modelMetadataService ?? new ModelMetadataService();
    const modelRegistry = new HostModelRegistry(
        options.credentials,
        options.modelSettingsStore ?? new MemoryEditorModelSettingsStore(),
        metadata,
    );
    const editorSettings = options.editorSettingsStore ?? new MemoryEditorSettingsStore();
    const projects =
        options.projectService ??
        new HostProjectService(options.projectDirectory, options.pickProjectDirectory);
    const nativeSession = options.nativeSession ?? new RuntimeSession(options.runtimeExecutable ? new NativeRuntime(options.runtimeExecutable) : undefined, async () => resolve(await projects.workspaceRoot(), "project.yaml"));
    const commands = options.commandRelay ?? new EditorCommandRelay();
    const images = createHostImageGeneration(projects, options.credentials, options.config, metadata);
    const luauLspArguments = [...(options.luauLspArguments ?? ["--stdio"])];
    if (options.luauDefinitionsIndex) {
        luauLspArguments.push("--definitions-index", options.luauDefinitionsIndex);
    }
    const luauLsp = new LuauLspGateway({
        executable:
            options.luauLspExecutable ??
            resolve(
                process.cwd(),
                "..",
                "build",
                "windows",
                "x64",
                "debug",
                "entisium-lsp.exe",
            ),
        arguments: luauLspArguments,
        workspaceRoot: () => projects.workspaceRoot(),
    });

    const server = createServer(async (request, response) => {
        const origin = request.headers.origin;
        if (origin && !allowedOrigins.has(origin)) {
            json(response, 403, { error: "Origin is not allowed." });
            return;
        }
        if (origin) {
            response.setHeader("Access-Control-Allow-Origin", origin);
            response.setHeader("Vary", "Origin");
        }
        response.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
        response.setHeader("Access-Control-Allow-Methods", "GET, PUT, DELETE, POST, OPTIONS");
        response.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
        response.setHeader("Cross-Origin-Opener-Policy", "same-origin");
        response.setHeader("X-Content-Type-Options", "nosniff");
        if (request.method === "OPTIONS") {
            response.writeHead(204);
            response.end();
            return;
        }

        const url = new URL(request.url ?? "/", `http://${request.headers.host ?? `${host}:${port}`}`);
        try {
            if (url.pathname === "/mcp") {
                if (!isLoopbackRequest(request)) {
                    json(response, 403, { error: "The Editor MCP endpoint is local-only." });
                    return;
                }
                await handleMcpRequest(
                    request,
                    response,
                    request.method === "POST" ? await readJson(request) : {},
                    commands,
                );
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/bootstrap") {
                const active = await modelRegistry.activeModel();
                const modelSettings = await modelRegistry.snapshot();
                json(response, 200, {
                    version: 1,
                    token,
                    project: await projects.snapshot(),
                    provider: {
                        id: active.provider.id,
                        name: active.provider.name,
                        configured: active.provider.configured,
                        model: modelForClient(active.model),
                    },
                    modelSettings,
                    reasoning: parseAgentSettings(options.config?.agent).reasoning,
                });
                return;
            }

            if (bearerToken(request) !== token && url.pathname.startsWith("/api/")) {
                json(response, 401, { error: "Invalid Editor Host token." });
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/sprite-animation") {
                const controller = new AbortController();
                const disconnected = () => { if (!response.writableEnded) controller.abort(); };
                response.once("close", disconnected);
                try {
                    const input = spriteAnimationSchema.parse(await readJson(request, 256 * 1024));
                    if (response.destroyed) controller.abort();
                    const service = new SpriteAnimationService(projects, pinned => {
                        const generator = createHostImageGeneration(pinned, options.credentials, options.config);
                        return options.generateImage ?? generator.generate.bind(generator);
                    }, createHostSpriteVideo(options.credentials));
                    const result = await service.invoke(input, controller.signal);
                    if (!response.destroyed) json(response, 200, result);
                } finally { response.removeListener("close", disconnected); }
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/image-generation") {
                const controller = new AbortController();
                const disconnected = () => { if (!response.writableEnded) controller.abort(); };
                response.once("close", disconnected);
                try {
                    const input = imageGenerationSchema.parse(await readJson(request, 128 * 1024));
                    if (response.destroyed) controller.abort();
                    controller.signal.throwIfAborted();
                    const result = await (options.generateImage ?? images.generate.bind(images))(input, controller.signal);
                    if (!response.destroyed) json(response, 200, result);
                } finally { response.removeListener("close", disconnected); }
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/native-runtime") {
                const controller = new AbortController();
                const disconnected = () => { if (!response.writableEnded) controller.abort(); };
                response.once("close", disconnected);
                try {
                    const body = asObject(await readJson(request));
                    if (response.destroyed) controller.abort();
                    if (typeof body.name !== "string") throw new Error("Native tool name is required.");
                    if (body.name === "runtime_play" && asObject(body.parameters).project !== undefined) {
                        throw new Error("Editor native runtime uses the current saved project; omit project.");
                    }
                    const result = await nativeSession.invoke(body.name, body.parameters, controller.signal);
                    if (!response.destroyed) json(response, 200, result);
                } finally { response.removeListener("close", disconnected); }
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/profile-symbols") {
                const moduleId = url.searchParams.get("module") ?? "";
                const match = /^wasm:([0-9a-f]{64})$/.exec(moduleId);
                if (!match) {
                    json(response, 400, { error: "Invalid profiling module identifier." });
                    return;
                }
                try {
                    const manifest = await loadProfileSymbolManifest(match[1]);
                    const requestedIds = url.searchParams.get("ids");
                    if (requestedIds === null) {
                        json(response, 200, manifest);
                        return;
                    }
                    const ids = [...new Set(requestedIds.split(","))];
                    if (
                        ids.length === 0 ||
                        ids.length > 512 ||
                        ids.some((id) => !/^\d+$/.test(id))
                    ) {
                        json(response, 400, { error: "Invalid profiling symbol identifiers." });
                        return;
                    }
                    const symbols: Record<string, unknown> = {};
                    for (const id of ids) {
                        if (Object.hasOwn(manifest.symbols, id)) {
                            symbols[id] = manifest.symbols[id];
                        }
                    }
                    json(response, 200, { ...manifest, symbols });
                } catch (error) {
                    if (
                        error &&
                        typeof error === "object" &&
                        "code" in error &&
                        error.code === "ENOENT"
                    ) {
                        json(response, 404, { error: "Profiling symbols are unavailable." });
                        return;
                    }
                    throw error;
                }
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/model-settings") {
                json(response, 200, await modelRegistry.snapshot());
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/model-settings/refresh") {
                json(response, 200, await modelRegistry.refreshModels(url.searchParams.get("provider") ?? "", url.searchParams.get("force") === "true"));
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/editor-settings") {
                json(response, 200, await editorSettings.read());
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/editor/commands") {
                commands.attach(request, response);
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/editor/commands/result") {
                const body = asObject(await readJson(request));
                const result = asObject(body.response) as unknown as EditorCommandResponse;
                if (
                    typeof body.id !== "string" ||
                    typeof result.requestId !== "string" ||
                    typeof result.ok !== "boolean"
                ) {
                    throw new Error("Editor command results require id and response fields.");
                }
                if (!commands.complete(body.id, result)) {
                    json(response, 404, { error: "Editor command is no longer pending." });
                    return;
                }
                json(response, 200, { ok: true });
                return;
            }

            if (request.method === "PUT" && url.pathname === "/api/v1/editor-settings") {
                const body = asObject(await readJson(request, 64 * 1024));
                const appearance = asObject(body.appearance);
                const next: EditorSettings = {
                    version: 1,
                    appearance: {
                        agentDensity:
                            appearance.agentDensity === "compact" ||
                            appearance.agentDensity === "comfortable"
                                ? appearance.agentDensity
                                : (() => {
                                      throw new Error("Agent density must be compact or comfortable.");
                                  })(),
                    },
                };
                await editorSettings.write(next);
                json(response, 200, next);
                return;
            }

            if (request.method === "PUT" && url.pathname === "/api/v1/model-settings") {
                const body = asObject(await readJson(request, 128 * 1024));
                json(
                    response,
                    200,
                    await modelRegistry.configure(body as unknown as ConfigureModelInput),
                );
                return;
            }

            if (request.method === "PUT" && url.pathname === "/api/v1/model-settings/provider") {
                const body = asObject(await readJson(request, 128 * 1024));
                json(
                    response,
                    200,
                    await modelRegistry.configureProvider(body as unknown as ConfigureProviderInput),
                );
                return;
            }

            if (request.method === "PUT" && url.pathname === "/api/v1/model-settings/model") {
                const body = asObject(await readJson(request, 128 * 1024));
                json(
                    response,
                    200,
                    await modelRegistry.configureRegistryModel(
                        body as unknown as ConfigureRegistryModelInput,
                    ),
                );
                return;
            }

            if (request.method === "DELETE" && url.pathname === "/api/v1/model-settings/credential") {
                json(
                    response,
                    200,
                    await modelRegistry.deleteCredential(url.searchParams.get("provider") ?? ""),
                );
                return;
            }

            if (request.method === "DELETE" && url.pathname === "/api/v1/model-settings/model") {
                json(
                    response,
                    200,
                    await modelRegistry.deleteModel(
                        url.searchParams.get("provider") ?? "",
                        url.searchParams.get("model") ?? "",
                    ),
                );
                return;
            }

            if (request.method === "DELETE" && url.pathname === "/api/v1/model-settings/provider") {
                json(
                    response,
                    200,
                    await modelRegistry.deleteProvider(url.searchParams.get("provider") ?? ""),
                );
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/project") {
                json(response, 200, await projects.snapshot());
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/project/open") {
                json(response, 200, await projects.open());
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/project/files") {
                json(response, 200, { files: await projects.list() });
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/project/inspect") {
                json(
                    response,
                    200,
                    await projects.inspect(url.searchParams.get("path") ?? ""),
                );
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/project/file") {
                const content = await projects.read(url.searchParams.get("path") ?? "");
                if (!content) {
                    json(response, 404, { error: "Project file not found." });
                    return;
                }
                response.writeHead(200, {
                    "Content-Type": "application/octet-stream",
                    "Content-Length": content.byteLength,
                    "Cache-Control": "no-store",
                });
                response.end(content);
                return;
            }

            if (request.method === "PUT" && url.pathname === "/api/v1/project/file") {
                const body = asObject(await readJson(request));
                if (typeof body.path !== "string" || typeof body.content !== "string") {
                    throw new Error("Project file writes require path and string content.");
                }
                await projects.write(body.path, body.content);
                json(response, 200, { ok: true });
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/project/directory") {
                const body = asObject(await readJson(request));
                if (typeof body.path !== "string") {
                    throw new Error("Project directory creation requires a path.");
                }
                await projects.createDirectory(body.path);
                json(response, 200, { ok: true });
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/v1/project/rename") {
                const body = asObject(await readJson(request, 64 * 1024));
                if (typeof body.source !== "string" || typeof body.destination !== "string") {
                    throw new Error("Project rename requires source and destination paths.");
                }
                await projects.rename(body.source, body.destination);
                json(response, 200, { ok: true });
                return;
            }

            if (request.method === "DELETE" && url.pathname === "/api/v1/project/file") {
                await projects.remove(url.searchParams.get("path") ?? "");
                json(response, 200, { ok: true });
                return;
            }

            if (request.method === "GET" && url.pathname === "/api/v1/project/events") {
                response.writeHead(200, {
                    "Content-Type": "text/event-stream; charset=utf-8",
                    "Cache-Control": "no-cache, no-transform",
                    Connection: "keep-alive",
                });
                response.flushHeaders();
                response.write(`data: ${JSON.stringify({ type: "ready" })}\n\n`);
                const unsubscribe = projects.subscribe((event) => {
                    if (!response.destroyed) response.write(`data: ${JSON.stringify(event)}\n\n`);
                });
                const heartbeat = setInterval(() => {
                    if (!response.destroyed) response.write(": heartbeat\n\n");
                }, 15_000);
                request.once("close", () => {
                    clearInterval(heartbeat);
                    unsubscribe();
                });
                return;
            }

            if (request.method === "POST" && url.pathname === "/api/stream") {
                const body = (await readJson(request)) as ProxyRequest;
                if (typeof body.model?.provider !== "string" || typeof body.model.id !== "string") {
                    json(response, 400, { error: "A registered model is required." });
                    return;
                }
                const abortController = new AbortController();
                response.on("close", () => {
                    if (!response.writableEnded) abortController.abort();
                });
                const model = await modelRegistry.getModel(body.model.provider, body.model.id, abortController.signal);
                if (!model) {
                    json(response, 400, { error: "Requested model is unavailable." });
                    return;
                }
                const context = sanitizeContext(body.context);
                response.writeHead(200, {
                    "Content-Type": "text/event-stream; charset=utf-8",
                    "Cache-Control": "no-cache, no-transform",
                    Connection: "keep-alive",
                });
                response.flushHeaders();
                const stream = modelRegistry.streamSimple(
                    model,
                    context,
                    sanitizeOptions(body.options, model, abortController.signal),
                );
                for await (const event of stream) {
                    response.write(`data: ${JSON.stringify(toProxyEvent(event))}\n\n`);
                }
                response.end();
                return;
            }

            if (url.pathname.startsWith("/runtime/")) {
                await serveRuntimeAsset(options.runtimeDirectory, url.pathname, response);
                return;
            }
            const profileSymbolPath = /^\/profile-symbols\/([0-9a-f]{64})\.json$/.exec(
                url.pathname,
            );
            if (request.method === "GET" && profileSymbolPath) {
                try {
                    json(response, 200, await loadProfileSymbolManifest(profileSymbolPath[1]));
                } catch (error) {
                    if (
                        error &&
                        typeof error === "object" &&
                        "code" in error &&
                        error.code === "ENOENT"
                    ) {
                        json(response, 404, { error: "Profiling symbols not found." });
                    } else {
                        throw error;
                    }
                }
                return;
            }
            await serveStatic(options.distDirectory, url.pathname, response);
        } catch (error) {
            if (response.headersSent) {
                if (!response.writableEnded && !response.destroyed) {
                    response.write(
                        `data: ${JSON.stringify({
                            type: "error",
                            reason: "error",
                            errorMessage: errorMessage(error),
                            usage: {
                                input: 0,
                                output: 0,
                                cacheRead: 0,
                                cacheWrite: 0,
                                totalTokens: 0,
                                cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
                            },
                        })}\n\n`,
                    );
                }
                response.end();
            } else {
                if (error instanceof ProjectPickerCancelledError) {
                    json(response, 409, { error: error.message, code: "cancelled" });
                } else {
                    json(response, 500, { error: errorMessage(error) });
                }
            }
        }
    });
    server.on("upgrade", (request, socket: Duplex, head) => {
        const url = new URL(request.url ?? "/", `http://${request.headers.host ?? `${host}:${port}`}`);
        const origin = request.headers.origin;
        if (
            url.pathname !== "/api/v1/lsp/luau" ||
            !isLoopbackRequest(request) ||
            (origin !== undefined && !allowedOrigins.has(origin)) ||
            url.searchParams.get("token") !== token
        ) {
            socket.end("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        void luauLsp.handleUpgrade(request, socket, head);
    });
    server.once("close", () => {
        luauLsp.dispose();
        commands.dispose();
        projects.dispose();
        void nativeSession.stop().catch((error) => console.error("Native runtime cleanup failed:", error));
    });

    return {
        server,
        token,
        stopRuntime: () => nativeSession.stop(),
        listen: () =>
            new Promise((resolveListen, reject) => {
                server.once("error", reject);
                server.listen(port, host, () => {
                    server.removeListener("error", reject);
                    const address = server.address() as AddressInfo;
                    resolveListen({ host, port: address.port });
                });
            }),
    };
}

async function serveStatic(directory: string, pathname: string, response: ServerResponse): Promise<void> {
    const root = resolve(directory);
    const requested = resolve(root, `.${decodeURIComponent(pathname)}`);
    if (requested !== root && !requested.startsWith(`${root}${sep}`)) {
        json(response, 403, { error: "Invalid asset path." });
        return;
    }
    let file = requested;
    try {
        const information = await stat(file);
        if (information.isDirectory()) file = join(file, "index.html");
    } catch {
        file = resolve(root, "index.html");
    }
    try {
        await access(file);
    } catch {
        json(response, 404, { error: "Editor assets have not been built." });
        return;
    }
    const contentType = contentTypes[extname(file).toLowerCase()] ?? "application/octet-stream";
    response.writeHead(200, { "Content-Type": contentType });
    createReadStream(file).pipe(response);
}

async function serveRuntimeAsset(
    directory: string,
    pathname: string,
    response: ServerResponse,
): Promise<void> {
    const name = decodeURIComponent(pathname.slice("/runtime/".length));
    if (
        name !== "index.html" &&
        !/^entisium-editor-runtime\.(?:data|js|wasm)$/.test(name)
    ) {
        json(response, 404, { error: "Unknown WebAssembly runtime asset." });
        return;
    }
    const file = resolve(directory, "runtime", name);
    try {
        await access(file);
    } catch {
        json(response, 404, {
            error: "WebAssembly runtime assets have not been built. Run the entisium-editor-runtime build first.",
        });
        return;
    }
    response.writeHead(200, {
        "Content-Type": contentTypes[extname(file).toLowerCase()] ?? "application/octet-stream",
    });
    createReadStream(file).pipe(response);
}
