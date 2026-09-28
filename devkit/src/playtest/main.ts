import { runPlaytest } from "./runner.js";
import { parseArgs } from "node:util";

const { positionals: [project, entry], values } = parseArgs({ allowPositionals: true, options: {
    record: { type: "string" }, replay: { type: "string" },
} });
if (!project || !entry) {
    console.error("Usage: npm run playtest -- <project.yaml> <test.luau> [--record tape.json | --replay tape.json]");
    process.exitCode = 1;
} else {
    const controller = new AbortController();
    const cancel = () => controller.abort();
    process.once("SIGINT", cancel);
    process.once("SIGTERM", cancel);
    try {
        if (values.record && values.replay) throw new Error("Choose either --record or --replay.");
        const result = await runPlaytest({ project, entry, signal: controller.signal,
            modelTape: values.record ? { mode: "record", path: values.record }
                : values.replay ? { mode: "replay", path: values.replay } : undefined,
            onLog: text => process.stderr.write(text + "\n") });
        console.log(`PASS (${result.operations} operations, frame ${result.frame})`);
    } catch (error) {
        console.error(error instanceof Error ? error.message : String(error));
        process.exitCode = 1;
    } finally {
        process.removeListener("SIGINT", cancel);
        process.removeListener("SIGTERM", cancel);
    }
}
