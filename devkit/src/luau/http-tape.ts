import { access, readFile, writeFile } from "node:fs/promises";
import { z } from "zod/v4";
import type { NativeHostConfiguration } from "./configuration.js";
const entry = z.object({
    request: z.object({ connection: z.string(), method: z.string(), path: z.string(), body: z.string() }).strict(),
    response: z.object({status: z.number().int().min(100).max(599), headers: z.array(z.tuple([z.string(), z.string()])), body: z.string()}).strict().optional(),
    error: z.string().optional(),
}).strict().refine(value => (value.response !== undefined) !== (value.error !== undefined));
const fileSchema = z.object({format: z.literal("entisium.http-tape"), version: z.literal(1), complete: z.literal(true), entries: z.array(entry).max(4096)}).strict();
export class HttpTape {
    private entries: z.infer<typeof entry>[] = [];
    private received = false;
    private constructor(private readonly mode: "record" | "replay", private readonly path: string) {}
    static async open(mode: "record" | "replay", path: string) {
        const tape = new HttpTape(mode, path);
        if (mode === "replay") {
            const source = await readFile(path, "utf8");
            if (Buffer.byteLength(source) > 32 * 1024 * 1024) throw new Error("HTTP tape exceeds 32 MiB.");
            tape.entries = fileSchema.parse(JSON.parse(source)).entries;
        } else {
            const exists = await access(path).then(() => true, (error: NodeJS.ErrnoException) => { if (error.code !== "ENOENT") throw error; return false; });
            if (exists) throw new Error("HTTP tape already exists.");
        }
        return tape;
    }
    configuration(): NonNullable<NativeHostConfiguration["tape"]> {
        return this.mode === "record" ? {mode: "record"} : {mode: "replay", entries: this.entries};
    }
    capture(value: unknown) {
        const result = z.object({version: z.literal(1), mode: z.literal(this.mode), consumed: z.number().int().nonnegative(), entries: z.array(entry).max(4096)}).strict().parse(value);
        if (this.mode === "record") this.entries = result.entries;
        this.received = true;
    }
    async close() {
        if (this.mode !== "record") return;
        const source = JSON.stringify({format: "entisium.http-tape", version: 1, complete: this.received, entries: this.entries}, null, 2);
        if (Buffer.byteLength(source) > 32 * 1024 * 1024) throw new Error("HTTP tape exceeds 32 MiB.");
        await writeFile(this.path, source, {flag: "wx", mode: 0o600});
    }
}
