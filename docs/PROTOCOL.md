# Engine <-> VS Code protocol

The GDExtension runs a TCP server in debug builds (default `127.0.0.1:27500`,
configurable via project setting `angelscript/tooling/port` and env var
`GDAS_TOOLING_PORT`). Both the language server and the debug adapter connect
to it, possibly at the same time (the server accepts multiple clients).

Framing: one JSON object per line, UTF-8, terminated by `\n`. Every message
has a `"type"` field. Unknown message types must be ignored.

## Handshake

Client sends first:

```json
{"type": "hello", "client": "vscode-lsp", "version": 1}
```

Engine replies:

```json
{"type": "welcome", "engine": "godot", "godot_version": "4.5.0", "extension_version": "0.1.0", "version": 1}
```

## Type database (IntelliSense)

Client: `{"type": "request_type_db"}`

Engine replies with a stream:

```json
{"type": "type_db_begin"}
{"type": "type_db_class", "name": "Node", "base": "Object", "native": true,
 "methods": [{"name": "add_child", "return": "void",
              "args": [{"name": "node", "type": "Node@", "default": null}],
              "static": false, "vararg": false}],
 "properties": [{"name": "position", "type": "Vector2"}],
 "signals": [{"name": "ready", "args": []}],
 "constants": [{"name": "NOTIFICATION_READY", "value": 13}],
 "enums": [{"name": "ProcessMode", "values": [{"name": "PROCESS_MODE_INHERIT", "value": 0}]}]}
{"type": "type_db_class", "name": "Player", "base": "CharacterBody2D", "native": false,
 "path": "res://player.as", "line": 1, "methods": [...], "properties": [...], "signals": [...]}
{"type": "type_db_end", "count": 812}
```

Types in the DB use AngelScript spellings (`int64`, `double`, `Vector2`,
`Node@`). Script classes are re-sent (single `type_db_class` message each)
whenever the shared module is rebuilt.

## Diagnostics

Pushed by the engine after every module rebuild or validation:

```json
{"type": "diagnostics", "path": "res://player.as",
 "items": [{"line": 10, "column": 4, "severity": "error", "message": "..."}]}
```

`severity` is `error` | `warning` | `info`. An empty `items` array clears
diagnostics for that path.

## Debugging

Breakpoints (replaces the full set for one file; reply confirms):

```json
{"type": "set_breakpoints", "path": "res://player.as", "lines": [10, 22]}
{"type": "breakpoints_set", "path": "res://player.as", "verified": [10, 22]}
```

Execution control (client -> engine, no direct reply; state changes are
events): `{"type": "continue"}`, `{"type": "pause"}`, `{"type": "step_over"}`,
`{"type": "step_in"}`, `{"type": "step_out"}`.

Engine events:

```json
{"type": "stopped", "reason": "breakpoint", "path": "res://player.as", "line": 10, "text": ""}
{"type": "continued"}
{"type": "exited"}
```

`reason` is `breakpoint` | `step` | `pause` | `exception` (then `text` holds
the exception message).

While stopped:

```json
{"type": "request_stack"}
{"type": "stack", "frames": [{"id": 0, "func": "_process", "path": "res://player.as", "line": 12}]}

{"type": "request_variables", "frame": 0, "scope": "locals"}
{"type": "variables", "frame": 0, "scope": "locals",
 "variables": [{"name": "delta", "value": "0.016", "type": "double"}]}

{"type": "request_evaluate", "frame": 0, "expression": "speed * 2"}
{"type": "evaluate", "value": "600", "value_type": "double"}
```

`scope` is `locals` | `members`. Evaluation support is limited (v1 resolves
plain local/member names only).

## Paths

The engine always sends Godot `res://` paths. Clients map them to disk by
replacing `res://` with the configured project root (defaults to the
workspace folder containing `project.godot`).
