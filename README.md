# AngelScript for Godot

[AngelScript](https://www.angelcode.com/angelscript/) as a first-class scripting
language for [Godot 4](https://godotengine.org), delivered as a GDExtension.
Statically typed, C++-like syntax, hot reload, editor integration, and a
VS Code extension with IntelliSense and a debugger.

```angelscript
class Player : CharacterBody2D {
    [export] float speed = 300;

    void _physics_process(float delta) {
        Vector2 dir = Input.get_vector("left", "right", "up", "down");
        velocity = dir * speed;
        move_and_slide();
    }
}
```

## Repository layout (monorepo)

| Path | Description |
| --- | --- |
| `extension/` | The GDExtension: C++ sources, AngelScript runtime, godot-cpp bindings |
| `addons/godot-angelscript/` | The shippable addon (`.gdextension` + built libraries) |
| `vscode/` | VS Code extension: language server (IntelliSense) + debug adapter |
| `demo/` | Godot demo project using the addon |
| `docs/` | Architecture and design notes |

## Features

- `ScriptLanguageExtension` implementation: `.as` files are real Godot scripts
  you can attach to nodes, with exported properties, signals, and virtual
  method callbacks (`_ready`, `_process`, ...).
- Full engine API surface: Godot classes, singletons, math types, and global
  scope registered with the AngelScript engine automatically via reflection.
- Editor integration: validation with inline errors, code completion, script
  templates, documentation, and hot reload.
- VS Code extension: syntax highlighting, IntelliSense driven by a live type
  database exported by the running engine, and a DAP debugger (breakpoints,
  stepping, call stacks, variable inspection).

## Building

Requirements: SCons 4.x, a C++17 compiler, git submodules initialized.

```sh
git submodule update --init --recursive
scons target=template_debug
```

Build outputs land in `addons/godot-angelscript/bin/`. Open `demo/` in Godot
4.5+ to try it, or run the headless smoke test:

```sh
godot --headless --path demo res://smoke.tscn
```

Note: in debug builds the extension mirrors the entire ClassDB into
AngelScript at startup (~13 s one-time cost per editor/game launch on a
desktop CPU). Bytecode caching to eliminate this is on the roadmap.

### VS Code extension

```sh
cd vscode
npm install
npm run build
```

## Status

Early development. See `docs/ARCHITECTURE.md` for the design and
`docs/ROADMAP.md` for GDScript-parity progress.

## License

MIT. AngelScript is zlib-licensed; Godot and godot-cpp are MIT.
