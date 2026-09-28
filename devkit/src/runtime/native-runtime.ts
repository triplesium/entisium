import { configuredLuauHost, encodeHostConfiguration, type NativeHostConfiguration } from "../luau/configuration.js";
import { spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { access, realpath } from "node:fs/promises";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { dirname, resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { z } from "zod/v4";

const capabilitySchema = z.object({
    id: z.string(), schema: z.string(), label: z.string(), description: z.string(),
    read_only: z.boolean(), request_schema: z.unknown(), response_schema: z.unknown(),
});
const envelopeSchema = z.object({ version: z.literal(1), session: z.string() });
const helloSchema = envelopeSchema.extend({
    process_id: z.number().int().positive(), project: z.string(), project_file: z.string(),
    inspections: z.array(capabilitySchema).max(256),
});
export const inspectionResponseSchema = envelopeSchema.extend({
    request_id: z.string(), ok: z.boolean(), payload: z.unknown(),
    error: z.object({ kind: z.string(), message: z.string() }).nullable(),
    attachment: z.object({
        content_type: z.literal("image/png"), encoding: z.literal("base64"), data: z.string(),
    }).optional(),
});
export type InspectionResponse = z.infer<typeof inspectionResponseSchema>;
type Hello = z.infer<typeof helloSchema>;
type Request = { version: 1; session: string; request_id: string; provider: string; schema: string; payload: unknown };
type Pending = { request: Request; sent: boolean; resolve(value: InspectionResponse): void; reject(error: Error): void };

async function readBody(request: IncomingMessage): Promise<unknown> {
    const chunks: Buffer[] = [];
    let size = 0;
    for await (const chunk of request) {
        size += chunk.length;
        if (size > 32 * 1024 * 1024) throw new Error("Runtime message exceeds 32 MiB.");
        chunks.push(Buffer.from(chunk));
    }
    return JSON.parse(Buffer.concat(chunks).toString("utf8"));
}

/** One private loopback controller and one owned child per MCP session. */
export class NativeRuntime {
    private session?: string;
    private child?: ChildProcess;
    private server?: ReturnType<typeof createServer>;
    private hello?: Hello;
    private pending?: Pending;
    private log = "";
    private phase = "stopped";
    private failure?: string;
    private heartbeat?: unknown;
    private ready?: { resolve(): void; reject(error: Error): void };
    private lifecycle: Promise<unknown> = Promise.resolve();

    private serialize<T>(operation: () => Promise<T>): Promise<T> {
        const result = this.lifecycle.then(operation);
        this.lifecycle = result.catch(() => {});
        return result;
    }

    constructor(
        private readonly executable: string,
        private readonly startupTimeoutMs = 30_000,
        private readonly inspectionTimeoutMs = 30_000,
        private readonly configuration?: NativeHostConfiguration,
    ) {}

    status() {
        return {
            state: this.phase, pid: this.child?.pid, project: this.hello?.project,
            project_file: this.hello?.project_file, hidden: true, error: this.failure,
            heartbeat: this.heartbeat, inspections: this.hello?.inspections ?? [],
        };
    }

    logs() { return { text: this.log }; }

    private fail(message: string) {
        this.failure = message;
        this.phase = "failed";
        this.ready?.reject(new Error(message));
        this.pending?.reject(new Error(message));
        this.pending = undefined;
    }

    private async handle(request: IncomingMessage, response: ServerResponse) {
        const reply = (status: number, value?: unknown) => {
            response.writeHead(status, { "Content-Type": "application/json" });
            response.end(value === undefined ? undefined : JSON.stringify(value));
        };
        try {
            if (request.headers.origin) { reply(403); return; }
            if (request.method === "GET" && request.url === "/api/v1/runtime/inspection/next") {
                if (request.headers["x-entisium-runtime-session"] !== this.session || !this.session) {
                    reply(403); return;
                }
                // The probe has a 500ms read timeout. Avoid an idle HTTP busy loop.
                if (!this.pending) await delay(100);
                if (this.pending && !this.pending.sent) {
                    this.pending.sent = true;
                    reply(200, this.pending.request);
                } else reply(204);
                return;
            }
            if (request.method !== "POST") { reply(404); return; }
            const body = await readBody(request);
            const envelope = envelopeSchema.parse(body);
            if (!this.session || envelope.session !== this.session) { reply(403); return; }
            switch (request.url) {
                case "/api/v1/runtime/hello": {
                    const hello = helloSchema.parse(body);
                    if (hello.process_id !== this.child?.pid) { reply(403); return; }
                    this.hello = hello;
                    this.ready?.resolve();
                    reply(200, {});
                    return;
                }
                case "/api/v1/runtime/heartbeat":
                    this.heartbeat = body;
                    reply(200, {});
                    return;
                case "/api/v1/runtime/goodbye":
                    if (this.phase !== "stopping") this.fail("Runtime disconnected.");
                    reply(200, {});
                    return;
                case "/api/v1/runtime/inspection/response": {
                    const result = inspectionResponseSchema.parse(body);
                    if (!this.pending || result.request_id !== this.pending.request.request_id || !this.pending.sent) {
                        reply(409); return;
                    }
                    const pending = this.pending;
                    this.pending = undefined;
                    pending.resolve(result);
                    reply(200, {});
                    return;
                }
                default: reply(404);
            }
        } catch (error) {
            reply(400, { error: error instanceof Error ? error.message : String(error) });
        }
    }

    start(projectFile: string) {
        return this.serialize(() => this.startOwned(projectFile));
    }

    private async startOwned(projectFile: string) {
        if (this.session) {
            throw new Error("A runtime session already exists. Stop it before starting another project.");
        }
        this.phase = "starting";
        this.session = randomUUID();
        this.log = "";
        this.failure = undefined;
        this.heartbeat = undefined;
        try {
            const project = await realpath(resolve(projectFile));
            await access(this.executable);
            const startup = encodeHostConfiguration(this.configuration ?? await configuredLuauHost(dirname(project)));
            this.server = createServer((request, response) => { void this.handle(request, response); });
            this.server.requestTimeout = 5_000;
            this.server.headersTimeout = 5_000;
            await new Promise<void>((resolveReady, reject) => {
                this.server!.once("error", reject);
                this.server!.listen(0, "127.0.0.1", () => {
                    this.server!.removeListener("error", reject);
                    resolveReady();
                });
            });
            this.server.on("error", (error) => this.fail(`Runtime controller failed: ${error.message}`));
            const address = this.server.address();
            if (!address || typeof address === "string") throw new Error("Missing runtime control port.");
            const environment = { ...process.env };
            delete environment.ETS_EXIT_AFTER_SECONDS;
            delete environment.ETS_EXIT_AFTER_FRAMES;
            delete environment.ETS_RUNTIME_BUILD_ID;
            const connected = new Promise<void>((resolveReady, reject) => {
                this.ready = { resolve: resolveReady, reject };
            });
            const timeout = setTimeout(() => this.ready?.reject(new Error("Runtime startup timed out.")), this.startupTimeoutMs);
            try {
                this.child = spawn(this.executable, ["--hidden", "--config-stdin", project], {
                    cwd: dirname(project), windowsHide: true, stdio: ["pipe", "pipe", "pipe"],
                    env: { ...environment, ETS_RUNTIME_CONTROL_PORT: String(address.port), ETS_RUNTIME_SESSION: this.session },
                });
                this.child.stdin?.on("error", () => this.fail("Runtime configuration pipe closed."));
                this.child.stdin?.end(startup);
                const append = (chunk: Buffer) => { this.log = (this.log + chunk.toString("utf8")).slice(-64 * 1024); };
                this.child.stdout?.on("data", append);
                this.child.stderr?.on("data", append);
                this.child.once("error", (error) => this.fail(`Runtime launch failed: ${error.message}`));
                this.child.once("exit", (code, signal) => {
                    if (this.phase !== "stopping") this.fail(`Runtime exited (${signal ?? code}). See runtime_logs.`);
                });
                await connected;
            } finally {
                clearTimeout(timeout);
                this.ready = undefined;
            }
            // Hello precedes game startup. A completed inspection proves the game is ready.
            const result = await this.inspect("play.interfaces", {});
            if (!result.ok) throw new Error(result.error?.message ?? "Runtime readiness inspection failed.");
            if (this.failure) throw new Error(this.failure);
            this.phase = "running";
            return this.status();
        } catch (error) {
            await this.stopOwned();
            this.phase = "failed";
            this.failure = error instanceof Error ? error.message : String(error);
            throw new Error(`${this.failure}\n${this.log.slice(-4000)}`);
        }
    }

    async inspect(provider: string, payload: unknown): Promise<InspectionResponse> {
        if (!this.hello || !this.session || this.failure || this.phase === "stopping") {
            throw new Error("Runtime is not ready. Start or restart it first.");
        }
        if (this.pending) throw new Error("A runtime operation is still in progress.");
        const capability = this.hello.inspections.find((item) => item.id === provider);
        if (!capability) throw new Error(`Runtime does not support ${provider}.`);
        if (Buffer.byteLength(JSON.stringify(payload)) > 64 * 1024) throw new Error("Inspection payload exceeds 64 KiB.");
        let timeout: NodeJS.Timeout | undefined;
        try {
            return await new Promise<InspectionResponse>((resolveResult, reject) => {
                this.pending = {
                    request: { version: 1, session: this.session!, request_id: randomUUID(), provider, schema: capability.schema, payload },
                    sent: false, resolve: resolveResult, reject,
                };
                timeout = setTimeout(() => {
                    // A timed-out mutation may already have run. Never retry it automatically.
                    this.fail("Runtime inspection timed out; execution is uncertain. Stop and restart the runtime.");
                }, this.inspectionTimeoutMs);
            });
        } finally { clearTimeout(timeout); }
    }

    stop(): Promise<void> {
        return this.serialize(() => this.stopOwned());
    }

    private async stopOwned() {
        this.phase = "stopping";
        this.ready?.reject(new Error("Runtime stopped."));
        this.pending?.reject(new Error("Runtime stopped."));
        this.pending = undefined;
        const child = this.child;
        if (child && child.exitCode === null && child.signalCode === null && child.pid) {
            await new Promise<void>((resolveExit, reject) => {
                const timeout = setTimeout(() => reject(new Error("Runtime process did not exit.")), 5_000);
                child.once("exit", () => { clearTimeout(timeout); resolveExit(); });
                child.kill();
            });
        }
        if (this.server) {
            const server = this.server;
            await new Promise<void>((resolveClosed) => {
                server.close(() => resolveClosed());
                server.closeAllConnections();
            });
        }
        this.child = undefined;
        this.server = undefined;
        this.hello = undefined;
        this.session = undefined;
        this.phase = "stopped";
    }
}
