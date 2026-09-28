import { emptyHostConfiguration, encodeHostConfiguration, type NativeHostConfiguration } from "./configuration.js";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { z } from "zod/v4";
import { HostTasks, type TaskDispatch } from "./tasks.js";

const eventSchema = z.discriminatedUnion("kind", [
    z.object({ kind: z.literal("request"), method: z.string(), payload: z.unknown() }).strict(),
    z.object({ kind: z.literal("completed"), tape: z.unknown().optional() }).strict(),
    z.object({ kind: z.literal("failed"), error: z.string(), tape: z.unknown().optional() }).strict(),
]);

export interface LuauHostOptions {
    executable: string;
    configuration?: NativeHostConfiguration;
    onTape?(value: unknown): void;
    sourceRoot: string;
    entry: string;
    /** @deprecated Libraries are embedded in the native host. */
    sdkDirectory?: string;
    dispatch(method: string, payload: unknown): Promise<unknown>;
    dispatchTask?: TaskDispatch;
    signal?: AbortSignal;
    timeoutMs?: number;
    onLog?(text: string): void;
    allowDuringCancellation?(method: string): boolean;
}

/** Transport only: coroutine scheduling belongs to the Luau execution host. */
export async function runLuau(options: LuauHostOptions): Promise<void> {
    options.signal?.throwIfAborted();
    const startup = encodeHostConfiguration(options.configuration ?? emptyHostConfiguration());
    const tasks = new HostTasks(options.dispatchTask);
    const child = spawn(options.executable, [options.sourceRoot, options.entry, "--config-stdin"], {
        windowsHide: true, stdio: ["pipe", "pipe", "pipe"],
    });
    let failure: Error | undefined;
    let completed = false;
    let processing = false;
    let forceKill: NodeJS.Timeout | undefined;
    const abort = (message: string) => {
        tasks.close();
        failure ??= new Error(message);
        // A suspended host request can receive cancellation and run Luau defers.
        // Busy scripts are stopped by the VM budget or this bounded fallback.
        forceKill ??= setTimeout(() => child.kill(), 1_000);
    };
    const onAbort = () => abort("Luau execution cancelled.");
    options.signal?.addEventListener("abort", onAbort, { once: true });
    if (options.signal?.aborted) onAbort();
    const timeout = setTimeout(() => abort("Luau execution timed out."), options.timeoutMs ?? 60_000);
    const lines = createInterface({ input: child.stdout });
    const exited = new Promise<void>((resolve, reject) => {
        child.once("error", reject);
        child.once("close", (code) => {
            if (failure) reject(failure);
            else if (completed && code === 0) resolve();
            else reject(new Error(`Luau host exited before successful completion (${code}).`));
        });
    });
    child.stderr.on("data", (chunk: Buffer) => options.onLog?.(chunk.toString("utf8")));
    child.stdin.on("error", (error) => { failure ??= error; child.kill(); });
    child.stdin.write(startup);
    lines.on("line", (line) => {
        if (!line.startsWith("ETS_LUAU:")) { options.onLog?.(line); return; }
        if (Buffer.byteLength(line) > 40 * 1024 * 1024) { abort("Luau event exceeds 40 MiB."); return; }
        void (async () => {
            try {
                const event = eventSchema.parse(JSON.parse(line.slice(9)));
                if (processing || completed) throw new Error("Out-of-order Luau host event.");
                if ((event.kind === "failed" || event.kind === "completed") && event.tape !== undefined) options.onTape?.(event.tape);
                if (event.kind === "failed") { failure ??= new Error(event.error); return; }
                if (event.kind === "completed") { completed = true; return; }
                if (Buffer.byteLength(line) > 2 * 1024 * 1024) throw new Error("Luau request exceeds 2 MiB.");
                processing = true;
                let reply: unknown;
                try {
                    const allowCleanup = event.method === "task.cancel" || (options.allowDuringCancellation?.(event.method) ?? false);
                    if (failure && !allowCleanup) throw failure;
                    const value = event.method.startsWith("task.")
                        ? await tasks.handle(event.method, event.payload)
                        : await options.dispatch(event.method, event.payload);
                    reply = failure && !allowCleanup
                        ? { ok: false, error: failure.message }
                        : { ok: true, value: value ?? {} };
                } catch (error) {
                    reply = { ok: false, error: error instanceof Error ? error.message : String(error) };
                }
                const encoded = JSON.stringify(reply);
                if (Buffer.byteLength(encoded) > 32 * 1024 * 1024) throw new Error("Host reply exceeds 32 MiB.");
                processing = false;
                if (child.exitCode === null && child.signalCode === null) child.stdin.write(encoded + "\n");
            } catch (error) {
                failure ??= error instanceof Error ? error : new Error(String(error));
                child.kill();
            }
        })();
    });
    try { await exited; }
    finally {
        tasks.close();
        clearTimeout(timeout);
        clearTimeout(forceKill);
        options.signal?.removeEventListener("abort", onAbort);
        lines.close();
        if (child.exitCode === null && child.signalCode === null) child.kill();
    }
}
