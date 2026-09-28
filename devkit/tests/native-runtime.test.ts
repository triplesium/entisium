import { emptyHostConfiguration } from "../src/luau/configuration.js";
import { EventEmitter } from "node:events";
import { realpath } from "node:fs/promises";
import { spawn } from "node:child_process";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { NativeRuntime } from "../src/runtime/native-runtime.js";

vi.mock("node:child_process", () => ({ spawn: vi.fn() }));
let runtime: NativeRuntime;
let child: EventEmitter & { pid: number; exitCode: number | null; signalCode: string | null; stdin: EventEmitter & {end: ReturnType<typeof vi.fn>}; stdout: EventEmitter; stderr: EventEmitter; kill: ReturnType<typeof vi.fn> };
let base: string;
let session: string;
beforeEach(() => {
    child = Object.assign(new EventEmitter(), {
        pid: 12345, exitCode: null as number | null, signalCode: null as string | null,
        stdin: Object.assign(new EventEmitter(), {end: vi.fn()}),
        stdout: new EventEmitter(), stderr: new EventEmitter(), kill: vi.fn(() => {
            child.exitCode = 0; child.emit("exit", 0, null); return true;
        }),
    });
    vi.mocked(spawn).mockReset().mockImplementation(((_command: unknown, _args: unknown, options: { env: Record<string, string> }) => {
        base = `http://127.0.0.1:${options.env.ETS_RUNTIME_CONTROL_PORT}`;
        session = options.env.ETS_RUNTIME_SESSION;
        return child;
    }) as typeof spawn);
    runtime = new NativeRuntime(process.execPath, 1000, 1000, emptyHostConfiguration());
});
afterEach(async () => { await runtime.stop(); });
const post = (path: string, body: object) => fetch(`${base}/api/v1/runtime/${path}`, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ version: 1, session, ...body }),
});
const poll = () => fetch(`${base}/api/v1/runtime/inspection/next`, { headers: { "X-Entisium-Runtime-Session": session } });
async function start() {
    const calls = vi.mocked(spawn).mock.calls.length;
    const started = runtime.start(process.execPath);
    await vi.waitFor(() => expect(spawn).toHaveBeenCalledTimes(calls + 1), { interval: 5 });
    expect((await post("hello", { process_id: child.pid, project: "test", project_file: process.execPath,
        inspections: ["play.interfaces", "play.step"].map((id) => ({ id, schema: `${id}.v1`, label: id, description: id, read_only: false, request_schema: {}, response_schema: {} })),
    })).status).toBe(200);
    const request = await (await poll()).json();
    await post("inspection/response", { request_id: request.request_id, ok: true, payload: { interfaces: [] }, error: null });
    await started;
}

it("launches hidden, verifies readiness, rejects foreign sessions and correlates responses", async () => {
    await start();
    expect(vi.mocked(spawn).mock.calls[0]?.[1]).toEqual(["--hidden", "--config-stdin", await realpath(process.execPath)]);
    expect(child.stdin.end).toHaveBeenCalledWith(JSON.stringify(emptyHostConfiguration()) + "\n");
    expect(runtime.status().state).toBe("running");
    expect((await post("heartbeat", { session: "wrong" })).status).toBe(403);
    expect((await post("heartbeat", { version: 2 })).status).toBe(400);
    const result = runtime.inspect("play.step", { action: {} });
    await expect(runtime.inspect("play.step", {})).rejects.toThrow("in progress");
    const request = await (await poll()).json();
    expect(request.provider).toBe("play.step");
    expect((await post("inspection/response", { request_id: "wrong", ok: true, payload: {}, error: null })).status).toBe(409);
    await post("inspection/response", { request_id: request.request_id, ok: false, payload: null, error: { kind: "invalid_request", message: "bad action" } });
    expect((await result).error?.message).toBe("bad action");
    await runtime.stop();
    expect(child.kill).toHaveBeenCalledOnce();
    expect(runtime.status().state).toBe("stopped");
});

it("marks timed-out execution uncertain and refuses further actions", async () => {
    runtime = new NativeRuntime(process.execPath, 1000, 80, emptyHostConfiguration());
    await start();
    await expect(runtime.inspect("play.step", {})).rejects.toThrow("execution is uncertain");
    await expect(runtime.inspect("play.step", {})).rejects.toThrow("not ready");
    expect(runtime.status().state).toBe("failed");
});

it("rejects pending calls on process exit and retains bounded logs", async () => {
    await start();
    child.stderr.emit("data", Buffer.from("x".repeat(70_000)));
    expect(runtime.logs().text.length).toBe(64 * 1024);
    const result = runtime.inspect("play.step", {});
    child.exitCode = 7; child.emit("exit", 7, null);
    await expect(result).rejects.toThrow("Runtime exited (7)");
    await runtime.stop();
    expect(runtime.logs().text).not.toBe("");
});

it("cleans up a startup timeout and can subsequently start again", async () => {
    runtime = new NativeRuntime(process.execPath, 60, 1000, emptyHostConfiguration());
    await expect(runtime.start(process.execPath)).rejects.toThrow("startup timed out");
    expect(child.kill).toHaveBeenCalledOnce();
    child.exitCode = null;
    await start();
    expect(runtime.status().state).toBe("running");
});
