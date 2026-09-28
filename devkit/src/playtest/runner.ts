import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";
import { parse } from "yaml";
import { z } from "zod/v4";
import { runLuau } from "../luau/host.js";
import { NativeRuntime } from "../runtime/native-runtime.js";
import { defaultRuntimeExecutable } from "../runtime/session.js";
import { configuredLuauHost, type NativeHostConfiguration } from "../luau/configuration.js";
import { HttpTape } from "../luau/http-tape.js";

export interface PlaytestOptions {
    project: string;
    entry: string;
    runtimeExecutable?: string;
    luauExecutable?: string;
    signal?: AbortSignal;
    timeoutMs?: number;
    onLog?(text: string): void;
    configuration?: NativeHostConfiguration;
    resolveCredential?: (provider: string, signal: AbortSignal) => Promise<string | undefined>;
    modelTape?: { mode: "record" | "replay"; path: string };
}

export async function runPlaytest(options: PlaytestOptions): Promise<{ operations: number; frame: number }> {
    const project = resolve(options.project);
    const config = z.object({ asset_directory: z.string().default("assets") }).parse(parse(await readFile(project, "utf8")));
    const runtimePath = options.runtimeExecutable ?? defaultRuntimeExecutable();
    const configuration = options.configuration ?? await configuredLuauHost(dirname(project), options.resolveCredential, options.signal);
    const runtime = new NativeRuntime(runtimePath, 30_000, 30_000, configuration);
    let started = false;
    let operations = 0;
    let frame = 0;
    const recording = options.modelTape;
    const tape = recording ? await HttpTape.open(recording.mode, recording.path) : undefined;
    try {
        await runLuau({
            executable: options.luauExecutable ?? process.env.ETS_LUAU_HOST_PATH
                ?? resolve(dirname(runtimePath), process.platform === "win32" ? "entisium-luau-host.exe" : "entisium-luau-host"),
            sourceRoot: resolve(dirname(project), config.asset_directory),
            entry: resolve(options.entry),
            sdkDirectory: fileURLToPath(new URL("../../../runtime/playtest/luau/", import.meta.url)),
            signal: options.signal, timeoutMs: options.timeoutMs, onLog: options.onLog,
            allowDuringCancellation: method => method === "game.input",
            configuration: tape ? {...configuration, tape: tape.configuration()} : configuration,
            onTape: value => tape?.capture(value),
            async dispatch(method, payload) {
                operations++;
                if (method === "game.start") {
                    z.object({}).strict().parse(payload);
                    if (started) throw new Error("Game already started.");
                    await runtime.start(project);
                    started = true;
                    return {};
                }
                if (method === "game.stop") {
                    await runtime.stop(); started = false; return {};
                }
                const providers: Record<string, string> = {
                    "game.input": "test.input", "game.advance": "test.advance",
                    "game.snapshot": "test.snapshot", "game.capture": "play.capture",
                };
                if (!providers[method]) throw new Error(`Unknown host method: ${method}`);
                if (!started) throw new Error("Start the game before using game operations.");
                if (method === "game.input") {
                    // An empty Luau table has no intrinsic array/object distinction.
                    payload = z.object({ keys: z.union([
                        z.array(z.string()).max(16),
                        z.object({}).strict().transform((): string[] => []),
                    ]) }).strict().parse(payload);
                }
                const response = await runtime.inspect(providers[method], payload);
                if (!response.ok) throw new Error(response.error?.message ?? "Game operation failed.");
                const result = response.payload as Record<string, unknown>;
                if (typeof result.frame === "number") frame = result.frame;
                if (method === "game.snapshot") {
                    const snapshotSchema = z.object({ resources: z.array(z.unknown()), entities: z.array(z.object({
                        entity: z.number(), components: z.array(z.unknown()),
                    })) });
                    const encode = (input: unknown) => {
                        const snapshot = snapshotSchema.parse(input);
                        return {
                        resources: snapshot.resources.map(value => JSON.stringify(value)),
                        entities: snapshot.entities.map(row => ({ ...row, components: row.components.map(value => JSON.stringify(value)) })),
                        };
                    };
                    const metadata = { frame: result.frame, simulation_time: result.simulation_time };
                    if (Array.isArray(result.batch)) return { ...metadata, batch: result.batch.map(encode) };
                    return { ...metadata, ...encode(result) };
                }
                if (response.attachment) return { ...result, image: response.attachment };
                return result;
            },
        });
        return { operations, frame };
    } finally {
        // Hard cancellation also closes the game, even if Luau cannot run cleanup.
        await runtime.stop();
        const logs = runtime.logs().text;
        if (logs) options.onLog?.(logs);
        await tape?.close();
    }
}
