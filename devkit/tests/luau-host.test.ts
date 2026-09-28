import { createServer } from "node:http";
import { once } from "node:events";
import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { runLuau } from "../src/luau/host.js";

const executable = process.env.ETS_LUAU_HOST_PATH ?? resolve("../build/windows/x64/release/entisium-luau-host.exe");
const root = resolve("tests/fixtures/playtest");
const sdkDirectory = resolve("../scripting/libraries/playtest");

describe.skipIf(!existsSync(executable))("standalone Luau host", () => {
    it("sends HTTP in the native process without forwarding requests to DevKit", async () => {
        let body = "";
        const server = createServer(async (request, response) => {
            for await (const chunk of request) body += chunk.toString();
            response.writeHead(418, { "Content-Type": "text/plain" });
            response.end("native-response");
        });
        server.listen(0, "127.0.0.1");
        await once(server, "listening");
        try {
            const address = server.address();
            if (!address || typeof address === "string") throw new Error("Expected TCP address");
            await runLuau({ executable, sourceRoot: root, entry: resolve(root, "http.luau"), sdkDirectory,
                async dispatch(method) {
                    expect(method).toBe("http-test.url");
                    return { url: `http://127.0.0.1:${address.port}/` };
                },
                async dispatchTask() { throw new Error("HTTP must not be forwarded to Node"); },
            });
            expect(body).toBe("native-request");
        } finally {
            server.closeAllConnections();
            await new Promise<void>((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
        }
    });
    it("decodes JSON for schema validation without host services", async () => {
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "json.luau"), sdkDirectory,
            async dispatch() { throw new Error("JSON must not call host services"); },
            async dispatchTask() { throw new Error("JSON must not call background services"); },
        });
    });
    it("runs the task core under an independent host", async () => {
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "task-core.luau"), sdkDirectory,
            async dispatch() { throw new Error("Core must not call Playtest"); },
        });
    });
    it("shares results, detaches waiters and closes owned scopes exactly once", async () => {
        let cancelled = 0;
        let cleanup = false;
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "task-lifecycle.luau"), sdkDirectory,
            async dispatchTask(method, _payload, signal) {
                if (method === "shared") {
                    await new Promise(resolve => setTimeout(resolve, 100));
                    expect(signal.aborted).toBe(false);
                    return { answer: 42 };
                }
                expect(method).toBe("never");
                return new Promise((_, reject) => signal.addEventListener("abort", () => {
                    cancelled++; reject(new Error("cancelled"));
                }, { once: true }));
            },
            async dispatch(method, payload) {
                expect(method).toBe("cleanup");
                expect(payload).toEqual({ name: "verified" });
                cleanup = true;
                return {};
            },
        });
        expect(cancelled).toBe(2);
        expect(cleanup).toBe(true);
    });
    it("validates standalone schemas without calling host services", async () => {
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "schema.luau"), sdkDirectory,
            async dispatch() { throw new Error("Schema must not call host services"); },
        });
    });
    it("cancels an outstanding service when the entire run is aborted and still runs cleanup", async () => {
        const controller = new AbortController();
        let cleaned = false;
        let aborted = false;
        await expect(runLuau({ executable, sourceRoot: root, entry: resolve(root, "cancel-task.luau"), sdkDirectory,
            signal: controller.signal, allowDuringCancellation: method => method === "cleanup",
            async dispatchTask(_method, _payload, signal) {
                return new Promise((_, reject) => {
                    signal.addEventListener("abort", () => { aborted = true; reject(new Error("aborted")); }, { once: true });
                    setTimeout(() => controller.abort(), 20);
                });
            },
            async dispatch(method) { expect(method).toBe("cleanup"); cleaned = true; return {}; },
        })).rejects.toThrow("cancelled");
        expect(aborted).toBe(true);
        expect(cleaned).toBe(true);
    });
    it("awaits background work without advancing simulation and cancels expired and abandoned tasks", async () => {
        let ticks = 0;
        let observed = false;
        let resolved = false;
        let cancelled = 0;
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "tasks.luau"), sdkDirectory,
            async dispatchTask(method, _payload, signal) {
                if (method === "failure") throw new Error("provider failed");
                if (method === "late") { await new Promise(resolve => setTimeout(resolve, 15)); return {}; }
                if (method === "never") return new Promise((_, reject) => {
                    signal.addEventListener("abort", () => { cancelled++; reject(new Error("cancelled")); }, { once: true });
                });
                await new Promise(resolve => setTimeout(resolve, 50));
                expect(ticks).toBe(0);
                expect(observed).toBe(true);
                resolved = true;
                return { answer: 42 };
            },
            async dispatch(method) {
                if (method === "observe") { observed = true; return {}; }
                expect(method).toBe("game.advance");
                expect(resolved).toBe(true);
                ticks++;
                return { ticks: 1, delta: 1 / 60, stopped: false };
            },
        });
        expect(ticks).toBe(2);
        expect(cancelled).toBe(4);
    });
    it("samples deduplicated context graphs and preserves query semantics and metadata", async () => {
        let batches = 0;
        let encoded: any;
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "context.luau"), sdkDirectory,
            async dispatch(method, input) {
                const payload = input as any;
                if (method === "encoded") { encoded = JSON.parse(payload.text); return {}; }
                expect(method).toBe("game.snapshot");
                expect(payload.batch).toBeInstanceOf(Array);
                if (batches++ === 0) expect(payload.batch).toHaveLength(3);
                return { frame: 12, simulation_time: 0.2, batch: payload.batch.map((request: any) => ({
                    resources: request.resources ? [request.optional ? "null" : '{"value":42}'] : [],
                    entities: request.components ? [
                        { entity: 1, components: ['{"value":9}'] },
                        { entity: 2, components: ['{"value":2}'] },
                        { entity: 3, components: ['{"value":2}'] },
                    ] : [],
                })) };
            },
        });
        expect(batches).toBe(4);
        expect(encoded.data.empty).toEqual([]);
        expect(encoded.data.b).toEqual({ value: 42 });
        expect(encoded.schema.fields.a.description).toBe("Score");
        expect(encoded.schema.fields.missing.optional).toBe(true);
    });
    it("imports game types without installing plugins and shares one simulation clock", async () => {
        let ticks = 0;
        const cleaned: string[] = [];
        await runLuau({ executable, sourceRoot: root, entry: resolve(root, "scheduler.luau"), sdkDirectory,
            async dispatch(method, input) {
                const payload = input as any;
                if (method === "game.advance") { ticks++; return { ticks: 1, delta: 1 / 60, stopped: false }; }
                if (method === "game.snapshot") {
                    expect(payload.resources[0].name).toBe("project.types.State");
                    return { resources: ['{"value":42}'] };
                }
                if (method === "cleanup") { cleaned.push(payload.name); return {}; }
                throw new Error(`Unexpected method ${method}`);
            },
        });
        expect(ticks).toBe(4);
        expect(cleaned).toEqual(["child", "root"]);
    });

    it("fails on a child error and runs all registered cleanup", async () => {
        const cleaned: string[] = [];
        await expect(runLuau({ executable, sourceRoot: root, entry: resolve(root, "failure.luau"), sdkDirectory,
            async dispatch(method, payload) {
                if (method === "cleanup") cleaned.push((payload as any).name);
                return { ticks: 1, delta: 1 / 60, stopped: false };
            },
        })).rejects.toThrow("child failed");
        expect(cleaned).toEqual(["child", "root"]);
    });

    it("interrupts runaway scripts", async () => {
        await expect(runLuau({ executable, sourceRoot: root, entry: resolve(root, "runaway.luau"), sdkDirectory,
            async dispatch() { throw new Error("Unexpected request"); },
        })).rejects.toThrow("budget");
    });

    it("cancels a suspended test and permits input cleanup", async () => {
        const controller = new AbortController();
        let cleaned = false;
        await expect(runLuau({ executable, sourceRoot: root, entry: resolve(root, "cancel.luau"), sdkDirectory,
            signal: controller.signal,
            allowDuringCancellation: method => method === "game.input",
            async dispatch(method) {
                if (method === "game.advance") {
                    controller.abort();
                    return { ticks: 1, delta: 1 / 60, stopped: false };
                }
                if (method === "game.input") cleaned = true;
                return {};
            },
        })).rejects.toThrow("cancelled");
        expect(cleaned).toBe(true);
    });
});
