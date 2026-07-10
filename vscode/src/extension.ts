import * as path from "node:path";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";
import { DEFAULT_HOST, DEFAULT_PORT } from "./common/protocol";

let client: LanguageClient | undefined;

function currentSettings(): { enginePort: number; engineHost: string; projectRoot: string } {
  const config = vscode.workspace.getConfiguration("godotAngelscript");
  return {
    enginePort: config.get<number>("enginePort", DEFAULT_PORT),
    engineHost: config.get<string>("engineHost", DEFAULT_HOST),
    projectRoot: config.get<string>("projectRoot", ""),
  };
}

/** Finds the folder containing project.godot in the workspace, if any. */
async function findProjectRoot(preferredFolder?: vscode.WorkspaceFolder): Promise<string | undefined> {
  const configured = currentSettings().projectRoot;
  if (configured.length > 0) {
    return configured;
  }
  const files = await vscode.workspace.findFiles("**/project.godot", "**/node_modules/**", 10);
  if (files.length === 0) {
    return preferredFolder?.uri.fsPath;
  }
  if (preferredFolder !== undefined) {
    const inFolder = files.find((file) => file.fsPath.startsWith(preferredFolder.uri.fsPath));
    if (inFolder !== undefined) {
      return path.dirname(inFolder.fsPath);
    }
  }
  return path.dirname(files[0].fsPath);
}

class GodotAngelscriptDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
  async resolveDebugConfiguration(
    folder: vscode.WorkspaceFolder | undefined,
    config: vscode.DebugConfiguration
  ): Promise<vscode.DebugConfiguration | undefined> {
    // F5 with no launch.json: synthesize an attach configuration.
    if (config.type === undefined && config.request === undefined && config.name === undefined) {
      const editor = vscode.window.activeTextEditor;
      if (editor === undefined || editor.document.languageId !== "angelscript") {
        return undefined;
      }
      config.type = "godot-angelscript";
      config.request = "attach";
      config.name = "Attach to running Godot game";
    }
    const settings = currentSettings();
    if (config.port === undefined) {
      config.port = settings.enginePort;
    }
    if (config.hostname === undefined) {
      config.hostname = settings.engineHost;
    }
    if (config.projectRoot === undefined || config.projectRoot === "") {
      config.projectRoot = await findProjectRoot(folder);
    }
    return config;
  }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  const serverModule = context.asAbsolutePath(path.join("dist", "server.js"));
  const serverOptions: ServerOptions = {
    run: { module: serverModule, transport: TransportKind.ipc },
    debug: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: { execArgv: ["--nolazy", "--inspect=6009"] },
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "angelscript" }],
    synchronize: { configurationSection: "godotAngelscript" },
    initializationOptions: { settings: currentSettings() },
  };

  client = new LanguageClient(
    "godotAngelscript",
    "Godot AngelScript",
    serverOptions,
    clientOptions
  );
  await client.start();

  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider(
      "godot-angelscript",
      new GodotAngelscriptDebugConfigurationProvider()
    )
  );
}

export async function deactivate(): Promise<void> {
  if (client !== undefined) {
    await client.stop();
    client = undefined;
  }
}
