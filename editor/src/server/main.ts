import { resolve } from "node:path";
import { loadHostConfiguration } from "@entisium/agent/host/configuration";
import { runtimeExecutableFromConfig } from "@entisium/devkit/settings/runtime";
import { FileEditorSettingsStore } from "./editor-settings-store.js";
import {
    chooseProjectDirectory,
    projectDirectoryFromArguments,
} from "./native-directory-picker.js";
import { createEditorHost } from "./server.js";

function portFromEnvironment(): number {
    const value = Number.parseInt(process.env.ETS_EDITOR_HOST_PORT ?? "3100", 10);
    if (!Number.isInteger(value) || value < 1 || value > 65_535) {
        throw new Error("ETS_EDITOR_HOST_PORT must be a valid TCP port.");
    }
    return value;
}

const runtimeDirectory = resolve(
    process.env.ETS_EDITOR_RUNTIME_DIR ??
        resolve(process.cwd(), "..", "build", "wasm", "wasm32", "debug"),
);
const projectDirectory = projectDirectoryFromArguments(process.argv.slice(2));
const configuration = await loadHostConfiguration(undefined, projectDirectory);
const host = createEditorHost({
    credentials: configuration.credentials,
    editorSettingsStore: new FileEditorSettingsStore(),
    modelSettingsStore: configuration.modelSettingsStore,
    config: configuration.config,
    runtimeExecutable: runtimeExecutableFromConfig(configuration.config, configuration.store),
    distDirectory: resolve(process.cwd(), "dist", "browser"),
    runtimeDirectory,
    host: "127.0.0.1",
    port: portFromEnvironment(),
    projectDirectory,
    pickProjectDirectory: chooseProjectDirectory,
    luauLspExecutable: process.env.ETS_ENTISIUM_LSP_PATH?.trim() || undefined,
    luauDefinitionsIndex:
        process.env.ETS_ENTISIUM_LUAU_DEFINITIONS_INDEX?.trim() ||
        resolve(runtimeDirectory, "luau-definitions", "entisium-editor-runtime", "index.json"),
});

const address = await host.listen();
console.log(`[entisium editor] local host listening on http://${address.host}:${address.port}`);
console.log(`[entisium editor] MCP endpoint available at http://${address.host}:${address.port}/mcp`);

let shuttingDown = false;
async function shutdown(): Promise<void> {
    if (shuttingDown) return;
    shuttingDown = true;
    try { await host.stopRuntime(); }
    catch (error) { console.error(error); process.exitCode = 1; }
    host.server.close(() => process.exit(process.exitCode ?? 0));
    host.server.closeAllConnections();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
