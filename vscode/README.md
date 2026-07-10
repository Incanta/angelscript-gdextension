# Godot AngelScript for VS Code

IDE support for AngelScript scripts (`.as` files) used in the Godot game
engine via the AngelScript GDExtension. The extension talks to the tooling
server hosted by the running engine (TCP, default `127.0.0.1:27500`, see
`docs/PROTOCOL.md` in the repository root) to provide:

- Syntax highlighting for AngelScript, including Godot metadata annotations
  such as `[export]`, `[signal]`, `[tool]`, and `[export_range(...)]`
- Live diagnostics streamed from the engine after every module rebuild
- Completion, hover, go to definition, and signature help backed by the
  engine's type database (with a lightweight offline fallback)
- Debugging: attach to a running game, breakpoints, stepping, call stacks,
  locals/members, and hover evaluation

## Requirements

- VS Code 1.85 or newer
- A Godot project using the AngelScript GDExtension, running a debug build
  (the tooling server is only started in debug builds)

## Setup

1. Install the extension (or run it from source, see Development below).
2. Open the folder containing your Godot project. The extension locates
   `project.godot` automatically; if your layout is unusual, set
   `godotAngelscript.projectRoot` to the folder that contains it.
3. Start your game from the Godot editor (debug build). The language server
   connects automatically and retries every 5 seconds, so start order does
   not matter.

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `godotAngelscript.enginePort` | `27500` | TCP port of the engine tooling server |
| `godotAngelscript.engineHost` | `127.0.0.1` | Host of the engine tooling server |
| `godotAngelscript.projectRoot` | `""` | Folder containing `project.godot`; used to map `res://` paths to disk. When empty, the workspace is searched. |

If you changed the engine port (project setting `angelscript/tooling/port` or
the `GDAS_TOOLING_PORT` env var), mirror it in `godotAngelscript.enginePort`
and in your launch configuration.

## Debugging

Run your game, then attach with a launch configuration like this
(`.vscode/launch.json`):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "godot-angelscript",
      "request": "attach",
      "name": "Attach to running Godot game",
      "hostname": "127.0.0.1",
      "port": 27500,
      "projectRoot": "${workspaceFolder}"
    }
  ]
}
```

You can also press F5 in an `.as` file with no `launch.json`; the extension
synthesizes the same attach configuration. Breakpoints, step over/in/out,
pause, call stacks, Locals and Members scopes, and evaluating plain
local/member names in the Debug Console and on hover are supported. The
engine resolves only simple names in v1 of the protocol, so complex watch
expressions may fail.

## Development

Node.js 18+ is required.

```sh
cd vscode
npm install
npm run build       # bundles dist/extension.js, dist/server.js, dist/debugAdapter.js
npm run typecheck   # tsc --noEmit
npm test            # protocol smoke test against a fake engine server
npm run watch       # rebuild on change
```

Then open the `vscode/` folder in VS Code and press F5 ("Run Extension") to
launch an Extension Development Host, or package with `vsce package`.

### Layout

- `src/extension.ts`: extension entry point; starts the language client
- `src/server/`: language server (`server.ts`), engine connection with
  auto-reconnect (`engineConnection.ts`), in-memory type database
  (`typedb.ts`), and the regex-based type inference (`heuristics.ts`)
- `src/debugAdapter.ts`: Debug Adapter Protocol implementation
- `src/common/protocol.ts`: wire protocol types, `res://` path mapping, and
  the line-delimited JSON socket shared by the LSP and DAP sides
- `syntaxes/`, `language-configuration.json`: TextMate grammar and editor
  behavior for AngelScript
- `scripts/protocol-smoke.mjs`: smoke test speaking the real wire protocol
