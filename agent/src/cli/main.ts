import { parseArgs } from "node:util";
import { dirname, resolve } from "node:path";
import { mkdir, writeFile } from "node:fs/promises";
import { randomUUID } from "node:crypto";
import { HostModelRegistry } from "../models/model-registry.js";
import { ModelMetadataService } from "@entisium/devkit/models/service";
import { loadHostConfiguration } from "../host/configuration.js";
import { NativeRuntime } from "@entisium/devkit/runtime/native-runtime";
import { runtimeExecutableFromConfig } from "@entisium/devkit/settings/runtime";
import { HostProjectService } from "@entisium/devkit/workspace/project-service";
import { RuntimeSession } from "@entisium/devkit/runtime/session";
import { createNativeAgentTools } from "../tools/runtime.js";
import { createProjectTools } from "../tools/project.js";
import { runAgentTask } from "../core/task.js";
import { createImageGenerationTools } from "../tools/image-generation.js";
import { createHostImageGeneration } from "../host/image-generation.js";
import { SpriteAnimationService } from "@entisium/devkit/sprite-animation/service";
import { createSpriteAnimationTools } from "../tools/sprite-animation.js";
import { createHostSpriteVideo } from "../host/sprite-video.js";

async function main() {
    const { values } = parseArgs({ options: {
        project: { type: "string" }, prompt: { type: "string" },
        provider: { type: "string" }, model: { type: "string" },
        output: { type: "string" }, config: { type: "string" }, help: { type: "boolean" },
    } });
    if (values.help) {
        console.log("Usage: npm run agent -- --project <project.yaml> --prompt <task> [--provider <id> --model <id>] [--output <directory>] [--config <config.yaml>]\nUses config.yaml (or legacy model settings when absent); no Editor, browser or MCP server is required. Build entisium-runtime-host first.");
        return;
    }
    if (!values.project || !values.prompt?.trim()) throw new Error("--project and --prompt are required. Use --help.");
    if (Boolean(values.provider) !== Boolean(values.model)) throw new Error("Supply both --provider and --model.");
    const projectFile = resolve(values.project);
    const project = new HostProjectService(dirname(projectFile));
    const hostConfig = await loadHostConfiguration(values.config, dirname(projectFile));
    const session = new RuntimeSession(new NativeRuntime(runtimeExecutableFromConfig(hostConfig.config, hostConfig.store)), async () => projectFile);
    const abort = new AbortController();
    const interrupt = () => abort.abort(new Error("Agent task interrupted."));
    process.once("SIGINT", interrupt);
    process.once("SIGTERM", interrupt);
    try {
        // Validate workspace before contacting a model.
        if (projectFile !== resolve(await project.workspaceRoot(), "project.yaml")) throw new Error("--project must name project.yaml.");
        const credentials = hostConfig.credentials;
        const metadata = new ModelMetadataService();
        const registry = new HostModelRegistry(credentials, hostConfig.modelSettingsStore, metadata);
        const images = createHostImageGeneration(project, credentials, hostConfig.config, metadata);
        const sprites = new SpriteAnimationService(project, pinned => {
            const service = createHostImageGeneration(pinned, credentials, hostConfig.config, metadata);
            return service.generate.bind(service);
        }, createHostSpriteVideo(credentials));
        const model = values.provider && values.model
            ? await registry.getModel(values.provider, values.model, abort.signal) : (await registry.activeModel(abort.signal)).model;
        if (!model) throw new Error("The requested provider has no supported chat connection.");
        const output = resolve(values.output ?? resolve(dirname(projectFile), ".entisium", "agent-runs", randomUUID()));
        await mkdir(output, { recursive: true });
        let capture = 0;
        await runAgentTask({
            prompt: values.prompt,
            configuration: { model, streamFn: registry.streamSimple.bind(registry), reasoning: hostConfig.agent.reasoning },
            tools: [
                ...createProjectTools(project),
                ...createSpriteAnimationTools(sprites.invoke.bind(sprites)),
                ...createImageGenerationTools(async (input, signal) => {
                    const result = await images.generate(input, signal);
                    console.log(JSON.stringify({ type: "artifact", ...result, path: resolve(dirname(projectFile), result.path) }));
                    return result;
                }),
                ...createNativeAgentTools(async (name, parameters, signal) => {
                    const result = await session.invoke(name, parameters, signal);
                    if (result.image) {
                        const path = resolve(output, `capture-${++capture}.png`);
                        await writeFile(path, Buffer.from(result.image.data, "base64"));
                        console.log(JSON.stringify({ type: "artifact", path, mimeType: result.image.mimeType }));
                    }
                    return result;
                }),
            ],
            signal: abort.signal,
            onEvent: (event) => {
                if (event.type === "tool_execution_start") console.log(JSON.stringify({ type: event.type, tool: event.toolName }));
                if (event.type === "tool_execution_end") console.log(JSON.stringify({ type: event.type, tool: event.toolName, isError: event.isError,
                    content: event.result?.content?.filter((part: { type: string }) => part.type === "text") }));
                if (event.type === "message_end" && event.message.role === "assistant") {
                    for (const part of event.message.content) if (part.type === "text") console.log(JSON.stringify({ type: "assistant", text: part.text }));
                }
            },
            cleanup: () => session.stop(),
        });
    } finally {
        process.removeListener("SIGINT", interrupt);
        process.removeListener("SIGTERM", interrupt);
        try { await session.stop(); }
        finally { project.dispose(); }
    }
}
main().catch((error) => { console.error(error instanceof Error ? error.message : String(error)); process.exitCode = 1; });
